"""
AutoHDR (Decky) — backend.

Arquitectura:
  - La conversión SDR -> HDR10 la hace la capa Vulkan VK_LAYER_AUTOHDR
    (bin/libautohdr_layer_*.so) dentro del proceso del juego, por debajo de Wine.
    El juego "envía HDR", así que el gamescope de Anatase (que solo activa HDR con
    contenido HDR) pone el panel en HDR10 por sí solo.
  - Este backend instala el manifiesto de la capa, escribe su configuración en
    ~/.config/autohdr/ (global y por juego) y la capa la relee en caliente.
  - Solo toca gamescope para asegurar que el HDR de la pantalla está permitido
    (GAMESCOPE_DISPLAY_HDR_ENABLED), equivalente al interruptor HDR de Steam.
"""

import asyncio
import copy
import json
import os

import decky  # provisto por Decky Loader

from gamescope_x import GamescopeX
import drm_probe
import panel_hdr

USER_HOME = getattr(decky, "DECKY_USER_HOME", os.path.expanduser("~"))
PLUGIN_DIR = decky.DECKY_PLUGIN_DIR
SETTINGS_DIR = decky.DECKY_PLUGIN_SETTINGS_DIR
SETTINGS_FILE = os.path.join(SETTINGS_DIR, "settings.json")

LAYER_CONF_DIR = os.path.join(USER_HOME, ".config", "autohdr")
LAYER_APPS_DIR = os.path.join(LAYER_CONF_DIR, "apps")
LAYER_LOG = os.path.join(LAYER_CONF_DIR, "layer.log")
# ~/.local/share se busca antes que /usr/share: así la capa queda por encima de la
# capa WSI de gamescope (necesario para pedir el swapchain HDR10 a través de ella).
VK_LAYER_DIR = os.path.join(USER_HOME, ".local", "share", "vulkan", "implicit_layer.d")
LAYER_ARCHES = ("x86_64", "x86")

ATOM_HDR_ENABLED = "GAMESCOPE_DISPLAY_HDR_ENABLED"
ATOM_SUPPORTS_HDR = "GAMESCOPE_DISPLAY_SUPPORTS_HDR"
ATOM_HDR_FEEDBACK = "GAMESCOPE_HDR_OUTPUT_FEEDBACK"
# Restos de versiones anteriores del plugin que hay que dejar a 0 / borrar
ATOM_ITM_ENABLE = "GAMESCOPE_HDR_ITM_ENABLE"
ATOM_FORCE_HDR10 = "GAMESCOPE_DEBUG_FORCE_HDR10_PQ_OUTPUT"
ATOM_LOOK_PQ = "GAMESCOPE_COLOR_LOOK_PQ"
ATOM_LOOK_G22 = "GAMESCOPE_COLOR_LOOK_G22"

# Valores por defecto tomados del ReShadePreset.ini de AutoHDR
DEFAULT_PROFILE = {
    "enabled": False,
    "sdr_nits": 200,       # HCRT_PAPER_WHITE_NITS
    "target_nits": 1000,   # HCRT_MAX_NITS
    "expand_gamut": True,  # HCRT_EXPAND_GAMUT (Vivid)
    "grading": False,
    "brightness": 0,
    "contrast": 6,         # HCRT_CONTRAST 0.06
    "saturation": 20,      # HCRT_SATURATION 0.2
    "temperature": 0,
    "gamma": 0,
}

DEFAULT_SETTINGS = {
    "global": dict(DEFAULT_PROFILE),
    "manage_hdr_output": True,
    "language": "auto",
    "games": {},
}

LANGUAGES = ("auto", "es", "en", "fr", "de", "it", "pt", "ca", "zh", "ja", "ru")


def _sanitize_profile(p: dict) -> dict:
    out = dict(DEFAULT_PROFILE)
    if isinstance(p, dict):
        for k in out:
            if k in p:
                out[k] = p[k]
    for k in ("enabled", "grading", "expand_gamut"):
        out[k] = bool(out[k])
    out["sdr_nits"] = int(min(max(int(out["sdr_nits"]), 80), 1000))
    out["target_nits"] = int(min(max(int(out["target_nits"]), 300), 10000))
    for k, lim in (("brightness", 100), ("contrast", 100), ("saturation", 100), ("gamma", 40)):
        out[k] = int(min(max(int(out[k]), -lim), lim))
    out["temperature"] = int(min(max(int(out["temperature"]), -3000), 3000))
    return out


def _layer_conf_text(p: dict) -> str:
    return (
        "# Generado por el plugin AutoHDR de Decky\n"
        f"enabled={1 if p['enabled'] else 0}\n"
        f"paper_white={p['sdr_nits']}\n"
        f"peak={p['target_nits']}\n"
        f"expand_gamut={1 if p['expand_gamut'] else 0}\n"
        f"grading={1 if p['grading'] else 0}\n"
        f"brightness={p['brightness']}\n"
        f"contrast={p['contrast']}\n"
        f"saturation={p['saturation']}\n"
        f"temperature={p['temperature']}\n"
        f"gamma={p['gamma']}\n"
    )


def _write_atomic(path: str, text: str):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


class Plugin:
    async def _main(self):
        self.lock = asyncio.Lock()
        self.x = GamescopeX(decky.logger)
        self.current_app = None
        self.status = {"panel_hdr": False, "game": False}
        self.hdr_forced = False
        self.apply_ok = False
        self.settings = self._load()
        try:
            self._install_layer()
            self._write_layer_configs()
        except Exception as e:  # noqa: BLE001
            decky.logger.error(f"No se pudo instalar la capa Vulkan: {e}")
        decky.logger.info("AutoHDR cargado (capa Vulkan)")
        try:
            await self._reapply()
        except Exception as e:  # noqa: BLE001
            decky.logger.error(f"Error en la aplicación inicial: {e}")
        self.watchdog = asyncio.get_event_loop().create_task(self._watchdog())

    async def _unload(self):
        if getattr(self, "watchdog", None):
            self.watchdog.cancel()
        if self.hdr_forced:
            try:
                await asyncio.to_thread(self.x.apply, {ATOM_HDR_ENABLED: 0}, {}, [])
            except Exception:  # noqa: BLE001
                pass

    async def _uninstall(self):
        for arch in LAYER_ARCHES:
            try:
                os.remove(os.path.join(VK_LAYER_DIR, f"autohdr_layer_{arch}.json"))
            except OSError:
                pass
        decky.logger.info("Manifiestos de la capa AutoHDR eliminados")

    # ---- Capa Vulkan ---------------------------------------------------------

    def _install_layer(self):
        os.makedirs(VK_LAYER_DIR, exist_ok=True)
        os.makedirs(LAYER_APPS_DIR, exist_ok=True)
        for arch in LAYER_ARCHES:
            lib = os.path.join(PLUGIN_DIR, "bin", f"libautohdr_layer_{arch}.so")
            manifest = {
                "file_format_version": "1.2.0",
                "layer": {
                    "name": f"VK_LAYER_AUTOHDR_{arch}",
                    "type": "GLOBAL",
                    "library_path": lib,
                    "api_version": "1.3.0",
                    "implementation_version": "1",
                    "description": "AutoHDR: SDR -> HDR10 (Decky)",
                    "functions": {
                        "vkNegotiateLoaderLayerInterfaceVersion": "autohdr_NegotiateLoaderLayerInterfaceVersion"
                    },
                    "disable_environment": {"DISABLE_AUTOHDR": "1"},
                },
            }
            _write_atomic(os.path.join(VK_LAYER_DIR, f"autohdr_layer_{arch}.json"),
                          json.dumps(manifest, indent=2) + "\n")
        decky.logger.info(f"Capa Vulkan registrada en {VK_LAYER_DIR}")

    def _write_layer_configs(self):
        os.makedirs(LAYER_APPS_DIR, exist_ok=True)
        _write_atomic(os.path.join(LAYER_CONF_DIR, "default.conf"), _layer_conf_text(self.settings["global"]))
        wanted = set()
        for appid, g in self.settings["games"].items():
            if g.get("custom"):
                wanted.add(f"{appid}.conf")
                _write_atomic(os.path.join(LAYER_APPS_DIR, f"{appid}.conf"), _layer_conf_text(g["profile"]))
        for f in os.listdir(LAYER_APPS_DIR):
            if f.endswith(".conf") and f not in wanted:
                try:
                    os.remove(os.path.join(LAYER_APPS_DIR, f))
                except OSError:
                    pass

    def _layer_log_tail(self, n=6):
        try:
            with open(LAYER_LOG, "r", encoding="utf-8", errors="replace") as f:
                return [l.rstrip() for l in f.readlines()[-n:]]
        except OSError:
            return []

    # ---- Persistencia --------------------------------------------------------

    def _load(self) -> dict:
        s = copy.deepcopy(DEFAULT_SETTINGS)
        try:
            with open(SETTINGS_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
            s["manage_hdr_output"] = bool(data.get("manage_hdr_output", True))
            lang = data.get("language", "auto")
            s["language"] = lang if lang in LANGUAGES else "auto"
            s["global"] = _sanitize_profile(data.get("global", {}))
            for appid, g in (data.get("games") or {}).items():
                s["games"][str(appid)] = {
                    "name": str(g.get("name", appid)),
                    "custom": bool(g.get("custom", False)),
                    "profile": _sanitize_profile(g.get("profile", {})),
                }
        except FileNotFoundError:
            pass
        except Exception as e:  # noqa: BLE001
            decky.logger.error(f"settings.json inválido, usando valores por defecto: {e}")
        return s

    def _save(self):
        _write_atomic(SETTINGS_FILE, json.dumps(self.settings, indent=2, ensure_ascii=False))
        self._write_layer_configs()

    # ---- Estado / gamescope ----------------------------------------------------

    def _effective_profile(self):
        if self.current_app:
            g = self.settings["games"].get(self.current_app["appid"])
            if g and g.get("custom"):
                return g["profile"], f"perfil de {g['name']}"
            return self.settings["global"], "perfil global"
        return None, "sin juego"

    def _any_enabled(self) -> bool:
        if self.settings["global"]["enabled"]:
            return True
        return any(g.get("custom") and g["profile"]["enabled"] for g in self.settings["games"].values())

    def _apply_gamescope(self):
        """Permite el HDR en gamescope y limpia restos de versiones anteriores."""
        cardinals = {ATOM_ITM_ENABLE: 0, ATOM_FORCE_HDR10: 0}
        if self.settings["manage_hdr_output"] and self._any_enabled():
            cardinals[ATOM_HDR_ENABLED] = 1
            self.hdr_forced = True
        elif self.hdr_forced:
            cardinals[ATOM_HDR_ENABLED] = 0
            self.hdr_forced = False
        self.x.apply(cardinals, {}, [ATOM_LOOK_PQ, ATOM_LOOK_G22])

    async def _reapply(self):
        async with self.lock:
            try:
                await asyncio.to_thread(self._apply_gamescope)
                self.apply_ok = True
            except Exception as e:  # noqa: BLE001
                was_ok = self.apply_ok
                self.apply_ok = False
                if was_ok:
                    decky.logger.error(f"No se pudo escribir en gamescope: {e}")
            await self._refresh_status()

    async def _refresh_status(self):
        """Estado neutro de idioma: la interfaz lo traduce. Nunca lanza excepciones."""
        try:
            await self._refresh_status_inner()
        except Exception as e:  # noqa: BLE001
            decky.logger.error(f"No se pudo leer el estado: {e}")
            self.status = {"panel_hdr": False, "game": bool(self.current_app)}

    async def _refresh_status_inner(self):
        p, _ = self._effective_profile()
        drm = await asyncio.to_thread(lambda: drm_probe.internal_summary(drm_probe.probe()))
        st = {"panel_hdr": bool(drm.get("hdr_signal_now")), "game": bool(self.current_app)}
        if self.current_app:
            g = self.settings["games"].get(self.current_app["appid"])
            custom = bool(g and g.get("custom"))
            st.update({
                "enabled": bool(p and p["enabled"]),
                "profile": "game" if custom else "global",
                "profile_name": g["name"] if custom else "",
                "sdr": p["sdr_nits"] if p else 0,
                "peak": p["target_nits"] if p else 0,
            })
        self.status = st

    def _layer_state(self) -> str:
        """Último estado conocido de la capa a partir de su log."""
        for line in reversed(self._layer_log_tail(40)):
            if "swapchain HDR10 activo" in line:
                return "active"
            if "no ofrece" in line:
                return "no_formats"
            if "error" in line or "falló" in line or "fallo" in line:
                return "error"
            if "config " in line:
                return "loaded"
        return "none"

    async def _watchdog(self):
        while True:
            await asyncio.sleep(3)
            try:
                if not self.apply_ok:
                    await self._reapply()
                    continue
                if self.settings["manage_hdr_output"] and self._any_enabled():
                    v = await asyncio.to_thread(self.x.read_cardinals, [ATOM_HDR_ENABLED])
                    if v[ATOM_HDR_ENABLED] == 0:
                        decky.logger.info("HDR de gamescope desactivado por otro proceso; reactivando")
                        await self._reapply()
            except asyncio.CancelledError:
                raise
            except Exception as e:  # noqa: BLE001
                decky.logger.error(f"watchdog: {e}")

    def _state(self) -> dict:
        return {
            "global": self.settings["global"],
            "manage_hdr_output": self.settings["manage_hdr_output"],
            "language": self.settings["language"],
            "games": self.settings["games"],
            "current_app": self.current_app,
            "status": self.status,
            "layer_state": self._layer_state(),
        }

    # ---- API del frontend --------------------------------------------------------

    async def get_state(self):
        await self._refresh_status()
        return self._state()

    async def set_global(self, profile: dict):
        self.settings["global"] = _sanitize_profile(profile)
        self._save()
        await self._reapply()
        return self._state()

    async def set_manage_hdr_output(self, value: bool):
        self.settings["manage_hdr_output"] = bool(value)
        self._save()
        await self._reapply()
        return self._state()

    async def set_language(self, lang: str):
        self.settings["language"] = lang if lang in LANGUAGES else "auto"
        self._save()
        return self._state()

    async def set_game(self, appid: str, name: str, custom: bool, profile: dict):
        appid = str(appid)
        self.settings["games"][appid] = {
            "name": name or appid,
            "custom": bool(custom),
            "profile": _sanitize_profile(profile),
        }
        self._save()
        await self._reapply()
        return self._state()

    async def remove_game(self, appid: str):
        self.settings["games"].pop(str(appid), None)
        self._save()
        await self._reapply()
        return self._state()

    async def app_started(self, appid: str, name: str):
        appid = str(appid)
        if appid in ("0", "", "None"):
            return self._state()
        self.current_app = {"appid": appid, "name": name or appid}
        g = self.settings["games"].get(appid)
        if g and name and g["name"] != name:
            g["name"] = name
            self._save()
        decky.logger.info(f"Juego iniciado: {appid} {name}")
        await self._reapply()
        return self._state()

    async def app_stopped(self, appid: str):
        if self.current_app and self.current_app["appid"] == str(appid):
            self.current_app = None
            decky.logger.info(f"Juego cerrado: {appid}")
            await self._refresh_status()
        return self._state()

    async def reapply(self):
        await self._reapply()
        return self._state()

    async def diagnose(self):
        """Informe completo en el log del plugin + veredicto."""
        atoms = await asyncio.to_thread(self.x.read_cardinals, [ATOM_SUPPORTS_HDR, ATOM_HDR_ENABLED, ATOM_HDR_FEEDBACK])
        drm = drm_probe.internal_summary(await asyncio.to_thread(drm_probe.probe))
        conn, edid = await asyncio.to_thread(panel_hdr.find_internal_edid)
        edid_info = panel_hdr.parse_edid(edid) if edid else {}
        manifests = {a: os.path.exists(os.path.join(VK_LAYER_DIR, f"autohdr_layer_{a}.json")) for a in LAYER_ARCHES}
        libs = {a: os.path.exists(os.path.join(PLUGIN_DIR, "bin", f"libautohdr_layer_{a}.so")) for a in LAYER_ARCHES}
        log_tail = self._layer_log_tail(25)
        lines = [
            "===== DIAGNOSTICO AutoHDR (capa Vulkan) =====",
            f"Estado: {self.status}",
            f"Juego actual: {self.current_app}",
            f"Manifiestos capa: {manifests} en {VK_LAYER_DIR}",
            f"Librerias capa: {libs}",
            f"Config capa: {sorted(os.listdir(LAYER_APPS_DIR)) if os.path.isdir(LAYER_APPS_DIR) else '-'}",
            f"Propiedades gamescope: {atoms}",
            f"EDID {conn}: {edid_info.get('vendor')} {edid_info.get('model')} hdr_pq={edid_info.get('hdr_pq')}",
            f"DRM panel interno: {drm}",
            "--- layer.log (ultimas lineas) ---",
            *log_tail,
            "=============================================",
        ]
        for l in lines:
            decky.logger.info(l)

        # Código de veredicto: la interfaz lo traduce
        layer = self._layer_state()
        if drm.get("hdr_signal_now"):
            verdict = "v_hdr"
        elif layer == "active":
            verdict = "v_sc_sdr"
        elif layer == "no_formats":
            verdict = "v_no_formats"
        elif layer != "none":
            verdict = "v_loaded"
        else:
            verdict = "v_none"
        return {"verdict": verdict, "connector": drm.get("connector") or "?",
                "colorspace": str(drm.get("colorspace_now"))}
