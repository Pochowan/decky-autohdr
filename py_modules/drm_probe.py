"""
Diagnóstico DRM (kernel) de los conectores de pantalla, vía libdrm + ctypes.

Responde a la pregunta clave: ¿puede el kernel enviar HDR10 al panel?
gamescope solo activa HDR10 si el conector tiene las propiedades
"Colorspace" y "HDR_OUTPUT_METADATA". Además se lee su valor actual
para saber si el panel está recibiendo señal HDR ahora mismo
(Colorspace = BT2020_RGB y HDR_OUTPUT_METADATA != 0).

Solo lectura: no requiere ser DRM master ni cambia nada.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import glob
import os

DRM_MODE_OBJECT_CONNECTOR = 0xC0C0C0C0
DRM_MODE_PROP_ENUM = 1 << 3
DRM_MODE_PROP_BLOB = 1 << 4

CONNECTOR_TYPES = {
    1: "VGA", 2: "DVI-I", 3: "DVI-D", 4: "DVI-A", 5: "Composite", 6: "SVIDEO", 7: "LVDS",
    8: "Component", 9: "DIN", 10: "DP", 11: "HDMI-A", 12: "HDMI-B", 13: "TV", 14: "eDP",
    15: "Virtual", 16: "DSI", 17: "DPI", 18: "Writeback", 19: "SPI", 20: "USB",
}
INTERESTING = ("Colorspace", "HDR_OUTPUT_METADATA", "max bpc", "vrr_capable", "abm level",
               "panel orientation", "content type")


class _Res(ctypes.Structure):
    _fields_ = [
        ("count_fbs", ctypes.c_int), ("fbs", ctypes.POINTER(ctypes.c_uint32)),
        ("count_crtcs", ctypes.c_int), ("crtcs", ctypes.POINTER(ctypes.c_uint32)),
        ("count_connectors", ctypes.c_int), ("connectors", ctypes.POINTER(ctypes.c_uint32)),
        ("count_encoders", ctypes.c_int), ("encoders", ctypes.POINTER(ctypes.c_uint32)),
        ("min_width", ctypes.c_uint32), ("max_width", ctypes.c_uint32),
        ("min_height", ctypes.c_uint32), ("max_height", ctypes.c_uint32),
    ]


class _ConnHead(ctypes.Structure):
    # Solo los primeros campos de drmModeConnector (los que usamos)
    _fields_ = [
        ("connector_id", ctypes.c_uint32), ("encoder_id", ctypes.c_uint32),
        ("connector_type", ctypes.c_uint32), ("connector_type_id", ctypes.c_uint32),
        ("connection", ctypes.c_int),
    ]


class _ObjProps(ctypes.Structure):
    _fields_ = [
        ("count_props", ctypes.c_uint32),
        ("props", ctypes.POINTER(ctypes.c_uint32)),
        ("prop_values", ctypes.POINTER(ctypes.c_uint64)),
    ]


class _PropEnum(ctypes.Structure):
    _fields_ = [("value", ctypes.c_uint64), ("name", ctypes.c_char * 32)]


class _Prop(ctypes.Structure):
    _fields_ = [
        ("prop_id", ctypes.c_uint32), ("flags", ctypes.c_uint32), ("name", ctypes.c_char * 32),
        ("count_values", ctypes.c_int), ("values", ctypes.POINTER(ctypes.c_uint64)),
        ("count_enums", ctypes.c_int), ("enums", ctypes.POINTER(_PropEnum)),
        ("count_blobs", ctypes.c_int), ("blob_ids", ctypes.POINTER(ctypes.c_uint32)),
    ]


def _lib():
    name = ctypes.util.find_library("drm") or "libdrm.so.2"
    d = ctypes.CDLL(name)
    d.drmModeGetResources.restype = ctypes.POINTER(_Res)
    d.drmModeGetResources.argtypes = [ctypes.c_int]
    d.drmModeFreeResources.argtypes = [ctypes.POINTER(_Res)]
    d.drmModeGetConnectorCurrent.restype = ctypes.POINTER(_ConnHead)
    d.drmModeGetConnectorCurrent.argtypes = [ctypes.c_int, ctypes.c_uint32]
    d.drmModeFreeConnector.argtypes = [ctypes.c_void_p]
    d.drmModeObjectGetProperties.restype = ctypes.POINTER(_ObjProps)
    d.drmModeObjectGetProperties.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_uint32]
    d.drmModeFreeObjectProperties.argtypes = [ctypes.POINTER(_ObjProps)]
    d.drmModeGetProperty.restype = ctypes.POINTER(_Prop)
    d.drmModeGetProperty.argtypes = [ctypes.c_int, ctypes.c_uint32]
    d.drmModeFreeProperty.argtypes = [ctypes.POINTER(_Prop)]
    return d


def probe() -> dict:
    """Devuelve {"connectors": [...], "error": str|None}."""
    try:
        d = _lib()
    except OSError as e:
        return {"connectors": [], "error": f"libdrm no disponible: {e}"}

    out = []
    errors = []
    for path in sorted(glob.glob("/dev/dri/card*")):
        try:
            fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
        except OSError:
            try:
                fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
            except OSError as e:
                errors.append(f"{path}: {e.strerror}")
                continue
        try:
            res = d.drmModeGetResources(fd)
            if not res:
                continue
            try:
                for i in range(res.contents.count_connectors):
                    cid = res.contents.connectors[i]
                    conn = d.drmModeGetConnectorCurrent(fd, cid)
                    if not conn:
                        continue
                    ctype = conn.contents.connector_type
                    entry = {
                        "card": os.path.basename(path),
                        "name": f"{CONNECTOR_TYPES.get(ctype, ctype)}-{conn.contents.connector_type_id}",
                        "connected": conn.contents.connection == 1,
                        "props": {},
                    }
                    d.drmModeFreeConnector(conn)
                    props = d.drmModeObjectGetProperties(fd, cid, DRM_MODE_OBJECT_CONNECTOR)
                    if props:
                        try:
                            for j in range(props.contents.count_props):
                                p = d.drmModeGetProperty(fd, props.contents.props[j])
                                if not p:
                                    continue
                                try:
                                    pname = p.contents.name.decode(errors="ignore")
                                    if pname not in INTERESTING:
                                        continue
                                    val = int(props.contents.prop_values[j])
                                    shown = val
                                    if p.contents.flags & DRM_MODE_PROP_ENUM:
                                        for k in range(p.contents.count_enums):
                                            en = p.contents.enums[k]
                                            if en.value == val:
                                                shown = en.name.decode(errors="ignore")
                                                break
                                    elif p.contents.flags & DRM_MODE_PROP_BLOB:
                                        shown = "blob" if val else "vacío"
                                    entry["props"][pname] = shown
                                finally:
                                    d.drmModeFreeProperty(p)
                        finally:
                            d.drmModeFreeObjectProperties(props)
                    out.append(entry)
            finally:
                d.drmModeFreeResources(res)
        finally:
            os.close(fd)
    return {"connectors": out, "error": "; ".join(errors) or None}


def internal_summary(result: dict) -> dict:
    """Resumen del panel interno conectado."""
    for c in result.get("connectors", []):
        if c["connected"] and (c["name"].startswith("eDP") or c["name"].startswith("DSI")):
            p = c["props"]
            has_cs = "Colorspace" in p
            has_md = "HDR_OUTPUT_METADATA" in p
            return {
                "connector": f'{c["card"]}/{c["name"]}',
                "has_colorspace": has_cs,
                "has_hdr_metadata": has_md,
                "kernel_hdr_capable": has_cs and has_md,
                "colorspace_now": p.get("Colorspace"),
                "hdr_metadata_now": p.get("HDR_OUTPUT_METADATA"),
                "hdr_signal_now": p.get("Colorspace") == "BT2020_RGB" and p.get("HDR_OUTPUT_METADATA") == "blob",
                "max_bpc": p.get("max bpc"),
                "all_props": p,
            }
    return {"connector": None, "kernel_hdr_capable": None}
