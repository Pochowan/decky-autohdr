/*
 * VK_LAYER_AUTOHDR — capa Vulkan implícita que convierte la salida SDR de un
 * juego en HDR10 (PQ / BT.2020) usando la matemática de AutoHDR.fx.
 *
 * Por qué una capa Vulkan y no ReShade:
 *   - Con Proton todo juego D3D9-12 termina en Vulkan (DXVK / vkd3d-proton).
 *   - Esta capa es una librería nativa de Linux que el loader de Vulkan carga
 *     por debajo de Wine (igual que MangoHud o la capa WSI de gamescope). No hay
 *     ninguna DLL de Windows inyectada que el anticheat pueda ver.
 *   - gamescope en Anatase solo pone el panel en HDR si la app envía HDR: al
 *     crear el swapchain como HDR10, gamescope activa el HDR10 del panel solo.
 *
 * Funcionamiento:
 *   vkCreateSwapchainKHR: si el juego pide un swapchain SDR y la superficie
 *     ofrece A2B10G10R10 + HDR10_ST2084, se crea el swapchain real en HDR10 y
 *     se entregan al juego imágenes intermedias con el formato que pidió.
 *   vkQueuePresentKHR: un compute shader lee la imagen del juego y escribe la
 *     versión HDR10 en la imagen real del swapchain; luego se presenta esa.
 *
 * Configuración (la escribe el plugin de Decky, se relee en caliente):
 *   $XDG_CONFIG_HOME/autohdr/apps/<SteamAppId>.conf  o  .../default.conf
 * Variables de entorno: AUTOHDR=0 desactiva, AUTOHDR=1 fuerza activado.
 */

#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "autohdr_spv.h"

#define EXPORT __attribute__((visibility("default")))
#define LAYER_NAME "VK_LAYER_AUTOHDR"

/* ------------------------------------------------------------------------ */
/* Log                                                                      */
/* ------------------------------------------------------------------------ */

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void config_dir(char *out, size_t n, const char *sub)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, n, "%s/autohdr%s", xdg, sub);
    else
        snprintf(out, n, "%s/.config/autohdr%s", home ? home : "/tmp", sub);
}

static void logf_(const char *fmt, ...)
{
    char path[512];
    config_dir(path, sizeof(path), "/layer.log");
    pthread_mutex_lock(&g_log_lock);
    FILE *f = fopen(path, "a");
    if (f) {
        char comm[64] = "?";
        FILE *c = fopen("/proc/self/comm", "r");
        if (c) {
            if (fgets(comm, sizeof(comm), c))
                comm[strcspn(comm, "\n")] = 0;
            fclose(c);
        }
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        fprintf(f, "[%02d:%02d:%02d] [%d %s] ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)getpid(), comm);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    pthread_mutex_unlock(&g_log_lock);
}

/* ------------------------------------------------------------------------ */
/* Configuración                                                            */
/* ------------------------------------------------------------------------ */

typedef struct {
    bool enabled;
    float paper_white, peak;
    bool grading;
    float brightness, contrast, saturation, temperature, gamma;
    bool expand_gamut;
} Config;

static const Config k_default_config = {
    .enabled = false, .paper_white = 200.f, .peak = 1000.f, .grading = false,
    .brightness = 0.f, .contrast = 0.f, .saturation = 0.f, .temperature = 0.f, .gamma = 0.f,
    .expand_gamut = true,
};

static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static Config g_cfg;
static bool g_cfg_loaded = false;
static char g_cfg_path[512];
static time_t g_cfg_mtime = 0;
static double g_cfg_last_check = 0;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static const char *app_id(void)
{
    const char *id = getenv("SteamAppId");
    if (!id || !*id || !strcmp(id, "0"))
        id = getenv("SteamGameId");
    return (id && *id) ? id : NULL;
}

static void pick_config_path(void)
{
    char p[512];
    const char *id = app_id();
    if (id) {
        char sub[128];
        snprintf(sub, sizeof(sub), "/apps/%s.conf", id);
        config_dir(p, sizeof(p), sub);
        if (access(p, R_OK) == 0) {
            snprintf(g_cfg_path, sizeof(g_cfg_path), "%s", p);
            return;
        }
    }
    config_dir(g_cfg_path, sizeof(g_cfg_path), "/default.conf");
}

static void parse_config(const char *path, Config *c)
{
    *c = k_default_config;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        float v;
        if (sscanf(line, " %63[^= ] = %f", key, &v) != 2)
            continue;
        if (!strcmp(key, "enabled")) c->enabled = v != 0.f;
        else if (!strcmp(key, "paper_white")) c->paper_white = v;
        else if (!strcmp(key, "peak")) c->peak = v;
        else if (!strcmp(key, "grading")) c->grading = v != 0.f;
        else if (!strcmp(key, "brightness")) c->brightness = v;
        else if (!strcmp(key, "contrast")) c->contrast = v;
        else if (!strcmp(key, "saturation")) c->saturation = v;
        else if (!strcmp(key, "temperature")) c->temperature = v;
        else if (!strcmp(key, "gamma")) c->gamma = v;
        else if (!strcmp(key, "expand_gamut")) c->expand_gamut = v != 0.f;
    }
    fclose(f);
    if (c->paper_white < 40.f) c->paper_white = 40.f;
    if (c->peak < c->paper_white * 1.05f) c->peak = c->paper_white * 1.05f;
}

/* Devuelve la configuración actual; la relee como mucho cada 0,5 s si cambió. */
static Config get_config(void)
{
    pthread_mutex_lock(&g_cfg_lock);
    double t = now_s();
    if (!g_cfg_loaded || t - g_cfg_last_check > 0.5) {
        g_cfg_last_check = t;
        pick_config_path();
        struct stat st;
        time_t mt = (stat(g_cfg_path, &st) == 0) ? st.st_mtime : 0;
        if (!g_cfg_loaded || mt != g_cfg_mtime) {
            parse_config(g_cfg_path, &g_cfg);
            g_cfg_mtime = mt;
            if (!g_cfg_loaded)
                logf_("config %s: enabled=%d paper_white=%.0f peak=%.0f", g_cfg_path, g_cfg.enabled,
                      g_cfg.paper_white, g_cfg.peak);
            g_cfg_loaded = true;
        }
    }
    Config c = g_cfg;
    pthread_mutex_unlock(&g_cfg_lock);

    const char *env = getenv("AUTOHDR");
    if (env && !strcmp(env, "1"))
        c.enabled = true;
    return c;
}

static bool layer_disabled_by_env(void)
{
    const char *env = getenv("AUTOHDR");
    return env && !strcmp(env, "0");
}

/* Balance de blancos relativo a D65 (port de WhiteBalance de AutoHDR.fx) */
static void temp_rgb(float k, float out[3])
{
    static const double warm[3][3] = {
        {0.0, -2902.1955373783176, -8257.7997278925690},
        {0.0, 1669.5803561666639, 2575.2827530017594},
        {1.0, 1.3302673723350029, 1.8993753891711275}};
    static const double cool[3][3] = {
        {1745.0425298314172, 1216.6168361476490, -8257.7997278925690},
        {-2666.3474220535695, -2173.1012343082230, 2575.2827530017594},
        {0.55995389139931482, 0.70381203140554553, 1.8993753891711275}};
    const double (*m)[3] = k < 6504.0 ? warm : cool;
    double kk = k < 1000.0 ? 1000.0 : (k > 40000.0 ? 40000.0 : k);
    for (int i = 0; i < 3; i++) {
        double v = m[0][i] / (kk + m[1][i]) + m[2][i];
        out[i] = (float)(v < 0 ? 0 : (v > 1 ? 1 : v));
    }
}

/* ------------------------------------------------------------------------ */
/* Push constants (deben coincidir con autohdr.comp)                         */
/* ------------------------------------------------------------------------ */

typedef struct {
    float wb[4];
    float paper_white, peak, brightness, contrast, saturation, gamma_in;
    uint32_t flags, pad;
    int32_t width, height;
} PushConstants;

enum { F_SRGB_VIEW = 1, F_EXPAND = 2, F_ENABLED = 4, F_GRADE = 8 };

/* ------------------------------------------------------------------------ */
/* Tablas de despacho y datos por objeto                                    */
/* ------------------------------------------------------------------------ */

static inline void *dkey(const void *dispatchable) { return *(void **)dispatchable; }

typedef struct InstanceData {
    void *key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
    struct InstanceData *next;
} InstanceData;

#define DEVICE_FUNCS(X)                                                                          \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(CreateSwapchainKHR)                  \
    X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(QueuePresentKHR) X(CreateImage)            \
    X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory)                \
    X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout)           \
    X(DestroyPipelineLayout) X(CreateShaderModule) X(DestroyShaderModule)                        \
    X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool)                         \
    X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets)                   \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer)   \
    X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdBindPipeline) X(CmdBindDescriptorSets)        \
    X(CmdPushConstants) X(CmdDispatch) X(QueueSubmit) X(CreateSemaphore) X(DestroySemaphore)     \
    X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(ResetCommandBuffer)

#define MAX_QUEUES 64

typedef struct DeviceData {
    void *key;
    VkDevice device;
    VkPhysicalDevice phys;
    InstanceData *inst;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkSetDeviceLoaderData set_loader_data;
#define X(n) PFN_vk##n n;
    DEVICE_FUNCS(X)
#undef X
    VkPhysicalDeviceMemoryProperties mem_props;
    VkQueueFamilyProperties families[32];
    uint32_t family_count;
    struct { VkQueue queue; uint32_t family; } queues[MAX_QUEUES];
    uint32_t queue_count;
    struct DeviceData *next;
} DeviceData;

typedef struct SwapchainData {
    VkSwapchainKHR swapchain;
    DeviceData *dd;
    uint32_t count;
    VkExtent2D extent;
    VkFormat app_format;
    bool srgb_view;
    VkImage *real, *fake;
    VkDeviceMemory *mem;
    VkImageView *src_view, *dst_view;
    VkDescriptorSet *sets;
    VkCommandBuffer *cmd;
    VkFence *fence;
    VkSemaphore *sem;
    VkCommandPool pool;
    uint32_t pool_family;
    VkSampler sampler;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderModule shader;
    VkPipeline pipeline;
    VkDescriptorPool dpool;
    bool warned_queue;
    struct SwapchainData *next;
} SwapchainData;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static InstanceData *g_instances;
static DeviceData *g_devices;
static SwapchainData *g_swapchains;

static InstanceData *find_instance(void *key)
{
    for (InstanceData *i = g_instances; i; i = i->next)
        if (i->key == key) return i;
    return NULL;
}

static DeviceData *find_device(void *key)
{
    for (DeviceData *d = g_devices; d; d = d->next)
        if (d->key == key) return d;
    return NULL;
}

static SwapchainData *find_swapchain(VkSwapchainKHR sc)
{
    for (SwapchainData *s = g_swapchains; s; s = s->next)
        if (s->swapchain == sc) return s;
    return NULL;
}

static InstanceData *lookup_instance(const void *dispatchable)
{
    pthread_mutex_lock(&g_lock);
    InstanceData *i = find_instance(dkey(dispatchable));
    pthread_mutex_unlock(&g_lock);
    return i;
}

static DeviceData *lookup_device(const void *dispatchable)
{
    pthread_mutex_lock(&g_lock);
    DeviceData *d = find_device(dkey(dispatchable));
    pthread_mutex_unlock(&g_lock);
    return d;
}

/* ------------------------------------------------------------------------ */
/* Instancia                                                                */
/* ------------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                           const VkAllocationCallbacks *pAllocator,
                                                           VkInstance *pInstance)
{
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    if (!chain)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = create(pCreateInfo, pAllocator, pInstance);
    if (r != VK_SUCCESS)
        return r;

    InstanceData *id = calloc(1, sizeof(*id));
    id->key = dkey(*pInstance);
    id->instance = *pInstance;
    id->gipa = gipa;
#define L(n) id->n = (PFN_vk##n)gipa(*pInstance, "vk" #n);
    L(DestroyInstance) L(GetPhysicalDeviceSurfaceFormatsKHR) L(GetPhysicalDeviceSurfaceCapabilitiesKHR)
    L(GetPhysicalDeviceMemoryProperties) L(GetPhysicalDeviceQueueFamilyProperties)
    L(GetPhysicalDeviceFormatProperties)
#undef L
    pthread_mutex_lock(&g_lock);
    id->next = g_instances;
    g_instances = id;
    pthread_mutex_unlock(&g_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
    pthread_mutex_lock(&g_lock);
    InstanceData **pp = &g_instances, *id = NULL;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->key == dkey(instance)) {
            id = *pp;
            *pp = id->next;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    if (id) {
        id->DestroyInstance(instance, pAllocator);
        free(id);
    }
}

/* ------------------------------------------------------------------------ */
/* Dispositivo                                                              */
/* ------------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *pCreateInfo,
                                                         const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    VkLayerDeviceCreateInfo *cb = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;
    while (cb && !(cb->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && cb->function == VK_LOADER_DATA_CALLBACK))
        cb = (VkLayerDeviceCreateInfo *)cb->pNext;
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    InstanceData *inst = lookup_instance(phys);
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(inst ? inst->instance : VK_NULL_HANDLE, "vkCreateDevice");
    VkResult r = create(phys, pCreateInfo, pAllocator, pDevice);
    if (r != VK_SUCCESS)
        return r;

    DeviceData *dd = calloc(1, sizeof(*dd));
    dd->key = dkey(*pDevice);
    dd->device = *pDevice;
    dd->phys = phys;
    dd->inst = inst;
    dd->gdpa = gdpa;
    dd->set_loader_data = cb ? cb->u.pfnSetDeviceLoaderData : NULL;
#define X(n) dd->n = (PFN_vk##n)gdpa(*pDevice, "vk" #n);
    DEVICE_FUNCS(X)
#undef X
    if (inst) {
        inst->GetPhysicalDeviceMemoryProperties(phys, &dd->mem_props);
        dd->family_count = 32;
        inst->GetPhysicalDeviceQueueFamilyProperties(phys, &dd->family_count, dd->families);
    }
    pthread_mutex_lock(&g_lock);
    dd->next = g_devices;
    g_devices = dd;
    pthread_mutex_unlock(&g_lock);
    return VK_SUCCESS;
}

static void destroy_swapchain_data(SwapchainData *s);

static VKAPI_ATTR void VKAPI_CALL layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    pthread_mutex_lock(&g_lock);
    DeviceData **pp = &g_devices, *dd = NULL;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->key == dkey(device)) {
            dd = *pp;
            *pp = dd->next;
            break;
        }
    /* swapchains huérfanos de este device */
    SwapchainData **sp = &g_swapchains;
    while (*sp) {
        if ((*sp)->dd == dd) {
            SwapchainData *s = *sp;
            *sp = s->next;
            destroy_swapchain_data(s);
        } else {
            sp = &(*sp)->next;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (dd) {
        dd->DestroyDevice(device, pAllocator);
        free(dd);
    }
}

static void record_queue(DeviceData *dd, VkQueue q, uint32_t family)
{
    if (!q) return;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < dd->queue_count; i++)
        if (dd->queues[i].queue == q) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
    if (dd->queue_count < MAX_QUEUES) {
        dd->queues[dd->queue_count].queue = q;
        dd->queues[dd->queue_count].family = family;
        dd->queue_count++;
    }
    pthread_mutex_unlock(&g_lock);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *pQueue)
{
    DeviceData *dd = lookup_device(device);
    dd->GetDeviceQueue(device, family, index, pQueue);
    record_queue(dd, *pQueue, family);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *pQueue)
{
    DeviceData *dd = lookup_device(device);
    dd->GetDeviceQueue2(device, info, pQueue);
    record_queue(dd, *pQueue, info->queueFamilyIndex);
}

static bool queue_family(DeviceData *dd, VkQueue q, uint32_t *family)
{
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < dd->queue_count; i++)
        if (dd->queues[i].queue == q) {
            *family = dd->queues[i].family;
            ok = true;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Swapchain                                                                */
/* ------------------------------------------------------------------------ */

static bool is_srgb_format(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

static int32_t find_memory_type(DeviceData *dd, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < dd->mem_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (dd->mem_props.memoryTypes[i].propertyFlags & want) == want)
            return (int32_t)i;
    for (uint32_t i = 0; i < dd->mem_props.memoryTypeCount; i++)
        if (bits & (1u << i))
            return (int32_t)i;
    return -1;
}

static void destroy_swapchain_data(SwapchainData *s)
{
    DeviceData *dd = s->dd;
    VkDevice d = dd->device;
    if (s->fence) {
        for (uint32_t i = 0; i < s->count; i++)
            if (s->fence[i]) dd->WaitForFences(d, 1, &s->fence[i], VK_TRUE, UINT64_MAX);
    }
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->fence && s->fence[i]) dd->DestroyFence(d, s->fence[i], NULL);
        if (s->sem && s->sem[i]) dd->DestroySemaphore(d, s->sem[i], NULL);
        if (s->src_view && s->src_view[i]) dd->DestroyImageView(d, s->src_view[i], NULL);
        if (s->dst_view && s->dst_view[i]) dd->DestroyImageView(d, s->dst_view[i], NULL);
        if (s->fake && s->fake[i]) dd->DestroyImage(d, s->fake[i], NULL);
        if (s->mem && s->mem[i]) dd->FreeMemory(d, s->mem[i], NULL);
    }
    if (s->pool) dd->DestroyCommandPool(d, s->pool, NULL);
    if (s->dpool) dd->DestroyDescriptorPool(d, s->dpool, NULL);
    if (s->pipeline) dd->DestroyPipeline(d, s->pipeline, NULL);
    if (s->shader) dd->DestroyShaderModule(d, s->shader, NULL);
    if (s->layout) dd->DestroyPipelineLayout(d, s->layout, NULL);
    if (s->dsl) dd->DestroyDescriptorSetLayout(d, s->dsl, NULL);
    if (s->sampler) dd->DestroySampler(d, s->sampler, NULL);
    free(s->real); free(s->fake); free(s->mem); free(s->src_view); free(s->dst_view);
    free(s->sets); free(s->cmd); free(s->fence); free(s->sem);
    free(s);
}

#define CHECK(expr)                                                     \
    do {                                                                \
        VkResult _r = (expr);                                           \
        if (_r != VK_SUCCESS) {                                         \
            logf_("error %d en %s (línea %d)", (int)_r, #expr, __LINE__); \
            return false;                                               \
        }                                                               \
    } while (0)

/* Crea imágenes intermedias, vistas, pipeline y sincronización. */
static bool setup_swapchain(SwapchainData *s, const VkSwapchainCreateInfoKHR *ci, VkFormat real_format,
                            const VkImageFormatListCreateInfo *app_format_list)
{
    DeviceData *dd = s->dd;
    VkDevice d = dd->device;
    uint32_t n = s->count;

    s->fake = calloc(n, sizeof(VkImage));
    s->mem = calloc(n, sizeof(VkDeviceMemory));
    s->src_view = calloc(n, sizeof(VkImageView));
    s->dst_view = calloc(n, sizeof(VkImageView));
    s->sets = calloc(n, sizeof(VkDescriptorSet));
    s->cmd = calloc(n, sizeof(VkCommandBuffer));
    s->fence = calloc(n, sizeof(VkFence));
    s->sem = calloc(n, sizeof(VkSemaphore));

    VkImageFormatListCreateInfo fl = {0};
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = ci->imageFormat,
        .extent = {ci->imageExtent.width, ci->imageExtent.height, 1},
        .mipLevels = 1,
        .arrayLayers = ci->imageArrayLayers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = ci->imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = ci->imageSharingMode,
        .queueFamilyIndexCount = ci->queueFamilyIndexCount,
        .pQueueFamilyIndices = ci->pQueueFamilyIndices,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
        ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (app_format_list) {
            fl.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
            fl.viewFormatCount = app_format_list->viewFormatCount;
            fl.pViewFormats = app_format_list->pViewFormats;
            ii.pNext = &fl;
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        CHECK(dd->CreateImage(d, &ii, NULL, &s->fake[i]));
        VkMemoryRequirements req;
        dd->GetImageMemoryRequirements(d, s->fake[i], &req);
        int32_t mt = find_memory_type(dd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) { logf_("sin tipo de memoria"); return false; }
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt};
        CHECK(dd->AllocateMemory(d, &ai, NULL, &s->mem[i]));
        CHECK(dd->BindImageMemory(d, s->fake[i], s->mem[i], 0));

        VkImageViewCreateInfo sv = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = s->fake[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = ci->imageFormat,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageViewUsageCreateInfo su = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                         .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
        sv.pNext = &su;
        CHECK(dd->CreateImageView(d, &sv, NULL, &s->src_view[i]));

        VkImageViewCreateInfo dv = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = s->real[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = real_format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        CHECK(dd->CreateImageView(d, &dv, NULL, &s->dst_view[i]));

        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        CHECK(dd->CreateFence(d, &fi, NULL, &s->fence[i]));
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CHECK(dd->CreateSemaphore(d, &si, NULL, &s->sem[i]));
    }

    VkSamplerCreateInfo smp = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.f,
    };
    CHECK(dd->CreateSampler(d, &smp, NULL, &s->sampler));

    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &s->sampler},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 2, .pBindings = b};
    CHECK(dd->CreateDescriptorSetLayout(d, &dl, NULL, &s->dsl));

    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1, .pSetLayouts = &s->dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    CHECK(dd->CreatePipelineLayout(d, &pl, NULL, &s->layout));

    VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = sizeof(autohdr_spv), .pCode = autohdr_spv};
    CHECK(dd->CreateShaderModule(d, &sm, NULL, &s->shader));

    VkComputePipelineCreateInfo cp = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = s->shader, .pName = "main"},
        .layout = s->layout,
    };
    CHECK(dd->CreateComputePipelines(d, VK_NULL_HANDLE, 1, &cp, NULL, &s->pipeline));

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, n}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = n, .poolSizeCount = 2, .pPoolSizes = ps};
    CHECK(dd->CreateDescriptorPool(d, &dp, NULL, &s->dpool));

    for (uint32_t i = 0; i < n; i++) {
        VkDescriptorSetAllocateInfo da = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->dpool, .descriptorSetCount = 1,
                                          .pSetLayouts = &s->dsl};
        CHECK(dd->AllocateDescriptorSets(d, &da, &s->sets[i]));
        VkDescriptorImageInfo src = {s->sampler, s->src_view[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo dst = {VK_NULL_HANDLE, s->dst_view[i], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->sets[i], .dstBinding = 0,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &src},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->sets[i], .dstBinding = 1,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dst},
        };
        dd->UpdateDescriptorSets(d, 2, w, 0, NULL);
    }
    return true;
}

/* (Re)crea el command pool para la familia de la cola de presentación. */
static bool ensure_pool(SwapchainData *s, uint32_t family)
{
    DeviceData *dd = s->dd;
    VkDevice d = dd->device;
    if (s->pool && s->pool_family == family)
        return true;
    if (s->pool) {
        for (uint32_t i = 0; i < s->count; i++)
            dd->WaitForFences(d, 1, &s->fence[i], VK_TRUE, UINT64_MAX);
        dd->DestroyCommandPool(d, s->pool, NULL);
        s->pool = VK_NULL_HANDLE;
    }
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = family};
    CHECK(dd->CreateCommandPool(d, &pi, NULL, &s->pool));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = s->count};
    CHECK(dd->AllocateCommandBuffers(d, &ai, s->cmd));
    for (uint32_t i = 0; i < s->count; i++) {
        if (dd->set_loader_data)
            dd->set_loader_data(d, s->cmd[i]);
        else
            *(void **)s->cmd[i] = *(void **)d;
    }
    s->pool_family = family;
    return true;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                               const VkAllocationCallbacks *pAllocator,
                                                               VkSwapchainKHR *pSwapchain)
{
    DeviceData *dd = lookup_device(device);
    /* Solo dentro de juegos lanzados por Steam (o forzado con AUTOHDR=1):
     * nunca en gamescope ni en otros procesos del sistema. */
    const char *force = getenv("AUTOHDR");
    bool is_game = app_id() != NULL || (force && !strcmp(force, "1"));
    if (!is_game || layer_disabled_by_env())
        return dd->CreateSwapchainKHR(device, ci, pAllocator, pSwapchain);
    Config cfg = get_config();
    bool want = cfg.enabled && dd->inst &&
                ci->imageColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (!want)
        return dd->CreateSwapchainKHR(device, ci, pAllocator, pSwapchain);

    /* ¿La superficie ofrece HDR10 PQ? (lo expone la capa WSI de gamescope) */
    VkFormat real_format = VK_FORMAT_UNDEFINED;
    uint32_t nf = 0;
    dd->inst->GetPhysicalDeviceSurfaceFormatsKHR(dd->phys, ci->surface, &nf, NULL);
    VkSurfaceFormatKHR *fmts = calloc(nf ? nf : 1, sizeof(*fmts));
    dd->inst->GetPhysicalDeviceSurfaceFormatsKHR(dd->phys, ci->surface, &nf, fmts);
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
            fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
            real_format = fmts[i].format;
    if (real_format == VK_FORMAT_UNDEFINED) {
        logf_("la superficie no ofrece A2B10G10R10 + HDR10_ST2084 (%u formatos):", nf);
        for (uint32_t i = 0; i < nf; i++)
            logf_("  formato %d colorspace %d", fmts[i].format, fmts[i].colorSpace);
        logf_("¿HDR desactivado en gamescope o la capa WSI de gamescope está por encima de AutoHDR?");
    }
    free(fmts);
    if (real_format == VK_FORMAT_UNDEFINED)
        return dd->CreateSwapchainKHR(device, ci, pAllocator, pSwapchain);

    VkFormatProperties fp;
    dd->inst->GetPhysicalDeviceFormatProperties(dd->phys, real_format, &fp);
    VkSurfaceCapabilitiesKHR caps;
    dd->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR(dd->phys, ci->surface, &caps);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !(caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT)) {
        logf_("el swapchain HDR10 no admite STORAGE (features 0x%x, usage 0x%x); sin AutoHDR",
              fp.optimalTilingFeatures, caps.supportedUsageFlags);
        return dd->CreateSwapchainKHR(device, ci, pAllocator, pSwapchain);
    }

    /* Copia modificada de la petición. Se quita la lista de formatos mutables
     * (pertenece al formato del juego, no al HDR10) desenlazándola un momento. */
    VkSwapchainCreateInfoKHR mod = *ci;
    mod.imageFormat = real_format;
    mod.imageColorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    mod.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT;
    mod.flags &= ~VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;

    const VkImageFormatListCreateInfo *app_fl = NULL;
    VkBaseOutStructure *prev = (VkBaseOutStructure *)&mod;
    while (prev->pNext) {
        if (prev->pNext->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            app_fl = (const VkImageFormatListCreateInfo *)prev->pNext;
            break;
        }
        prev = prev->pNext;
    }
    if (app_fl)
        prev->pNext = (VkBaseOutStructure *)app_fl->pNext;
    VkResult r = dd->CreateSwapchainKHR(device, &mod, pAllocator, pSwapchain);
    if (app_fl)
        prev->pNext = (VkBaseOutStructure *)app_fl;
    if (r != VK_SUCCESS) {
        logf_("fallo al crear el swapchain HDR10 (%d); se usa el SDR original", (int)r);
        return dd->CreateSwapchainKHR(device, ci, pAllocator, pSwapchain);
    }

    SwapchainData *s = calloc(1, sizeof(*s));
    s->swapchain = *pSwapchain;
    s->dd = dd;
    s->extent = ci->imageExtent;
    s->app_format = ci->imageFormat;
    s->srgb_view = is_srgb_format(ci->imageFormat);
    dd->GetSwapchainImagesKHR(device, *pSwapchain, &s->count, NULL);
    s->real = calloc(s->count, sizeof(VkImage));
    dd->GetSwapchainImagesKHR(device, *pSwapchain, &s->count, s->real);

    if (!setup_swapchain(s, ci, real_format, app_fl)) {
        logf_("no se pudo preparar el procesado; se vuelve al swapchain SDR");
        destroy_swapchain_data(s);
        dd->DestroySwapchainKHR(device, *pSwapchain, pAllocator);
        return dd->CreateSwapchainKHR(device, ci, pAllocator, pSwapchain);
    }

    pthread_mutex_lock(&g_lock);
    s->next = g_swapchains;
    g_swapchains = s;
    pthread_mutex_unlock(&g_lock);
    logf_("swapchain HDR10 activo: %ux%u, %u imágenes, formato del juego %d (sRGB=%d), app=%s",
          s->extent.width, s->extent.height, s->count, s->app_format, s->srgb_view,
          app_id() ? app_id() : "?");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                            const VkAllocationCallbacks *pAllocator)
{
    DeviceData *dd = lookup_device(device);
    pthread_mutex_lock(&g_lock);
    SwapchainData **pp = &g_swapchains, *s = NULL;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->swapchain == swapchain) {
            s = *pp;
            *pp = s->next;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    if (s)
        destroy_swapchain_data(s);
    dd->DestroySwapchainKHR(device, swapchain, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                                  uint32_t *pCount, VkImage *pImages)
{
    DeviceData *dd = lookup_device(device);
    pthread_mutex_lock(&g_lock);
    SwapchainData *s = find_swapchain(swapchain);
    pthread_mutex_unlock(&g_lock);
    if (!s)
        return dd->GetSwapchainImagesKHR(device, swapchain, pCount, pImages);
    if (!pImages) {
        *pCount = s->count;
        return VK_SUCCESS;
    }
    uint32_t n = *pCount < s->count ? *pCount : s->count;
    memcpy(pImages, s->fake, n * sizeof(VkImage));
    *pCount = n;
    return n < s->count ? VK_INCOMPLETE : VK_SUCCESS;
}

static void fill_push_constants(SwapchainData *s, PushConstants *pc)
{
    Config c = get_config();
    memset(pc, 0, sizeof(*pc));
    pc->wb[0] = pc->wb[1] = pc->wb[2] = pc->wb[3] = 1.f;
    if (c.grading && fabsf(c.temperature) >= 1.f) {
        float ref[3], cur[3];
        temp_rgb(6504.f, ref);
        temp_rgb(6504.f + c.temperature, cur);
        for (int i = 0; i < 3; i++)
            pc->wb[i] = cur[i] / (ref[i] > 1e-6f ? ref[i] : 1e-6f);
    }
    pc->paper_white = c.paper_white;
    pc->peak = c.peak;
    pc->brightness = c.grading ? c.brightness / 100.f : 0.f;
    pc->contrast = c.grading ? c.contrast / 100.f : 0.f;
    pc->saturation = c.grading ? c.saturation / 100.f : 0.f;
    pc->gamma_in = 2.2f + c.gamma / 100.f;
    pc->flags = (s->srgb_view ? F_SRGB_VIEW : 0) | (c.expand_gamut ? F_EXPAND : 0) |
                (c.enabled ? F_ENABLED : 0) | (c.grading ? F_GRADE : 0);
    pc->width = (int32_t)s->extent.width;
    pc->height = (int32_t)s->extent.height;
}

static bool record(SwapchainData *s, uint32_t idx)
{
    DeviceData *dd = s->dd;
    VkCommandBuffer cb = s->cmd[idx];
    dd->ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(dd->BeginCommandBuffer(cb, &bi));

    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier pre[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = s->fake[idx], .subresourceRange = range},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = s->real[idx], .subresourceRange = range},
    };
    dd->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                           0, NULL, 2, pre);

    PushConstants pc;
    fill_push_constants(s, &pc);
    dd->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->pipeline);
    dd->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 0, 1, &s->sets[idx], 0, NULL);
    dd->CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    dd->CmdDispatch(cb, (s->extent.width + 15) / 16, (s->extent.height + 15) / 16, 1);

    VkImageMemoryBarrier post[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = 0,
         .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = s->fake[idx], .subresourceRange = range},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = 0,
         .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = s->real[idx], .subresourceRange = range},
    };
    dd->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                           0, NULL, 2, post);
    CHECK(dd->EndCommandBuffer(cb));
    return true;
}

#define MAX_PRESENT_SC 8

static VKAPI_ATTR VkResult VKAPI_CALL layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
    DeviceData *dd = lookup_device(queue);
    uint32_t family = 0;
    bool have_family = queue_family(dd, queue, &family);
    if (!have_family) {
        /* Cola no registrada: se asume la primera familia gráfica (la universal) */
        for (uint32_t i = 0; i < dd->family_count; i++)
            if (dd->families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                family = i;
                have_family = true;
                break;
            }
    }

    VkSemaphore waits[MAX_PRESENT_SC];
    uint32_t nwaits = 0;
    bool first = true;

    for (uint32_t k = 0; k < pi->swapchainCount && k < MAX_PRESENT_SC; k++) {
        pthread_mutex_lock(&g_lock);
        SwapchainData *s = find_swapchain(pi->pSwapchains[k]);
        pthread_mutex_unlock(&g_lock);
        if (!s)
            continue;
        if (!have_family || family >= dd->family_count ||
            !(dd->families[family].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT))) {
            if (!s->warned_queue) {
                logf_("la cola de presentación no admite compute; sin procesado");
                s->warned_queue = true;
            }
            continue;
        }
        uint32_t idx = pi->pImageIndices[k];
        if (!ensure_pool(s, family))
            continue;
        dd->WaitForFences(dd->device, 1, &s->fence[idx], VK_TRUE, UINT64_MAX);
        if (!record(s, idx))
            continue;
        dd->ResetFences(dd->device, 1, &s->fence[idx]);

        VkPipelineStageFlags *stages = NULL;
        if (first && pi->waitSemaphoreCount) {
            stages = calloc(pi->waitSemaphoreCount, sizeof(*stages));
            for (uint32_t i = 0; i < pi->waitSemaphoreCount; i++)
                stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = first ? pi->waitSemaphoreCount : 0,
            .pWaitSemaphores = first ? pi->pWaitSemaphores : NULL,
            .pWaitDstStageMask = stages,
            .commandBufferCount = 1,
            .pCommandBuffers = &s->cmd[idx],
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &s->sem[idx],
        };
        VkResult r = dd->QueueSubmit(queue, 1, &si, s->fence[idx]);
        free(stages);
        if (r != VK_SUCCESS) {
            logf_("QueueSubmit falló (%d)", (int)r);
            return r;
        }
        first = false;
        waits[nwaits++] = s->sem[idx];
    }

    if (nwaits == 0)
        return dd->QueuePresentKHR(queue, pi);

    VkPresentInfoKHR mod = *pi;
    mod.waitSemaphoreCount = nwaits;
    mod.pWaitSemaphores = waits;
    return dd->QueuePresentKHR(queue, &mod);
}

/* ------------------------------------------------------------------------ */
/* Resolución de funciones y negociación con el loader                      */
/* ------------------------------------------------------------------------ */

static PFN_vkVoidFunction device_intercept(const char *name)
{
#define I(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)layer_##n;
    I(DestroyDevice) I(GetDeviceQueue) I(GetDeviceQueue2) I(CreateSwapchainKHR) I(DestroySwapchainKHR)
    I(GetSwapchainImagesKHR) I(QueuePresentKHR)
#undef I
    return NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL autohdr_GetDeviceProcAddr(VkDevice device, const char *name);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL autohdr_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)autohdr_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)autohdr_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)layer_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)layer_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)layer_CreateDevice;
    PFN_vkVoidFunction f = device_intercept(name);
    if (f) return f;
    if (!instance) return NULL;
    InstanceData *id = lookup_instance(instance);
    return id ? id->gipa(instance, name) : NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL autohdr_GetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)autohdr_GetDeviceProcAddr;
    PFN_vkVoidFunction f = device_intercept(name);
    if (f) return f;
    DeviceData *dd = lookup_device(device);
    return dd ? dd->gdpa(device, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL autohdr_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (v->loaderLayerInterfaceVersion >= 2) {
        v->pfnGetInstanceProcAddr = autohdr_GetInstanceProcAddr;
        v->pfnGetDeviceProcAddr = autohdr_GetDeviceProcAddr;
        v->pfnGetPhysicalDeviceProcAddr = NULL;
        v->loaderLayerInterfaceVersion = 2;
    }
    return VK_SUCCESS;
}
