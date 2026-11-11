"""
Habilitar HDR real en pantallas internas que gamescope no reconoce como HDR.

gamescope solo activa la salida HDR10 (BT.2020 + PQ hacia el panel) si:
  - el EDID del panel anuncia BT2020RGB + SMPTE ST2084, o
  - un script Lua de "known_displays" declara  hdr = { supported = true, eotf = pq }.

Muchas consolas portátiles (OneXPlayer, etc.) tienen paneles OLED HDR cuyo
EDID no lo anuncia; en Windows funciona por el driver del fabricante.
Este módulo lee el EDID del panel interno y genera un script de usuario en
~/.config/gamescope/scripts/ que lo declara HDR. gamescope lo carga al iniciar.
"""

from __future__ import annotations

import glob
import os

SCRIPT_NAME = "90-autohdr-internal-panel.lua"
ENTRY_KEY = "autohdr_internal_panel"


# ---------------------------------------------------------------------------
# EDID
# ---------------------------------------------------------------------------


def find_internal_edid():
    """Devuelve (connector, bytes) del panel interno conectado, o (None, None)."""
    cands = sorted(glob.glob("/sys/class/drm/card*-eDP-*")) + sorted(glob.glob("/sys/class/drm/card*-DSI-*"))
    for c in cands:
        try:
            with open(os.path.join(c, "status")) as f:
                if f.read().strip() != "connected":
                    continue
            with open(os.path.join(c, "edid"), "rb") as f:
                data = f.read()
            if len(data) >= 128:
                return os.path.basename(c), data
        except OSError:
            continue
    return None, None


def _parse_dtd(b: bytes):
    pc = b[0] | (b[1] << 8)
    if pc == 0:
        return None
    clock = pc * 10  # kHz
    ha = b[2] | ((b[4] & 0xF0) << 4)
    hb = b[3] | ((b[4] & 0x0F) << 8)
    va = b[5] | ((b[7] & 0xF0) << 4)
    vb = b[6] | ((b[7] & 0x0F) << 8)
    hso = b[8] | ((b[11] & 0xC0) << 2)
    hsw = b[9] | ((b[11] & 0x30) << 4)
    vso = (b[10] >> 4) | ((b[11] & 0x0C) << 2)
    vsw = (b[10] & 0x0F) | ((b[11] & 0x03) << 4)
    htotal, vtotal = ha + hb, va + vb
    if htotal == 0 or vtotal == 0:
        return None
    return {
        "clock": clock,
        "hdisplay": ha,
        "hsync_start": ha + hso,
        "hsync_end": ha + hso + hsw,
        "htotal": htotal,
        "vdisplay": va,
        "vsync_start": va + vso,
        "vsync_end": va + vso + vsw,
        "vtotal": vtotal,
        "vrefresh": round(clock * 1000.0 / (htotal * vtotal)),
    }


def parse_edid(data: bytes) -> dict:
    info = {
        "vendor": "",
        "product": 0,
        "model": "",
        "modes": [],
        "hdr_pq": False,
        "bt2020": False,
        "max_nits": None,
        "max_fall": None,
        "min_nits": None,
        "has_displayid": False,
    }
    if len(data) < 128 or data[0:8] != b"\x00\xff\xff\xff\xff\xff\xff\x00":
        return info

    v = (data[8] << 8) | data[9]
    info["vendor"] = "".join(chr(((v >> s) & 31) + 64) for s in (10, 5, 0))
    info["product"] = data[10] | (data[11] << 8)

    for off in (54, 72, 90, 108):
        d = data[off:off + 18]
        if d[0] == 0 and d[1] == 0:
            if d[3] == 0xFC:  # nombre del producto
                info["model"] = d[5:18].split(b"\x0a")[0].decode("ascii", "ignore").strip()
        else:
            m = _parse_dtd(d)
            if m:
                info["modes"].append(m)

    for i in range(1, len(data) // 128):
        blk = data[i * 128:(i + 1) * 128]
        if blk[0] == 0x70:
            info["has_displayid"] = True
            continue
        if blk[0] != 0x02:
            continue
        dtd_start = blk[2]
        p = 4
        while p < dtd_start and p < 127:
            hdr = blk[p]
            tag, ln = (hdr >> 5) & 7, hdr & 31
            payload = blk[p + 1:p + 1 + ln]
            if tag == 7 and ln >= 2:
                ext = payload[0]
                if ext == 0x05:  # Colorimetry
                    info["bt2020"] = bool(payload[1] & 0x80)
                elif ext == 0x06:  # HDR static metadata
                    info["hdr_pq"] = bool(payload[1] & 0x04)
                    if ln >= 4 and payload[3]:
                        info["max_nits"] = round(50.0 * 2 ** (payload[3] / 32.0), 1)
                    if ln >= 5 and payload[4]:
                        info["max_fall"] = round(50.0 * 2 ** (payload[4] / 32.0), 1)
                    if ln >= 6 and info["max_nits"]:
                        info["min_nits"] = round(info["max_nits"] * (payload[5] / 255.0) ** 2 / 100.0, 4)
            p += 1 + ln
        if dtd_start:
            q = dtd_start
            while q + 18 <= 127:
                m = _parse_dtd(blk[q:q + 18])
                if not m:
                    break
                info["modes"].append(m)
                q += 18
    return info


# ---------------------------------------------------------------------------
# Script Lua para gamescope
# ---------------------------------------------------------------------------


def _lua_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def generate_lua(info: dict, max_nits: float, max_fall: float, min_nits: float) -> str:
    # Modos del EDID con la resolución nativa (para conservar las tasas de refresco)
    modes = info["modes"]
    native = modes[0] if modes else None
    by_rate = {}
    if native:
        for m in modes:
            if m["hdisplay"] == native["hdisplay"] and m["vdisplay"] == native["vdisplay"]:
                by_rate.setdefault(m["vrefresh"], m)
    mode_lines = []
    for rate in sorted(by_rate):
        m = by_rate[rate]
        fields = ", ".join(f"{k} = {m[k]}" for k in (
            "clock", "hdisplay", "hsync_start", "hsync_end", "htotal",
            "vdisplay", "vsync_start", "vsync_end", "vtotal", "vrefresh"))
        mode_lines.append(f"    [{rate}] = {{ {fields} }},")
    rates = ", ".join(str(r) for r in sorted(by_rate))

    model_check = f' and display.model == {_lua_str(info["model"])}' if info["model"] else ""
    pretty = info["model"] or f'{info["vendor"]} 0x{info["product"]:04X}'

    return f"""-- Generado por el plugin de Decky "AutoHDR Gamescope".
-- Declara el panel interno {pretty} como compatible con HDR10 (PQ).
-- Para revertir: borra este archivo y reinicia Steam / la sesion de juego.

local edid_modes = {{
{chr(10).join(mode_lines)}
}}
local edid_rates = {{ {rates} }}

local entry = {{
    pretty_name = {_lua_str("AutoHDR: " + pretty)},
    hdr = {{
        supported = true,
        force_enabled = true,
        eotf = (gamescope.eotf and gamescope.eotf.pq) or 1,
        max_content_light_level = {max_nits},
        max_frame_average_luminance = {max_fall},
        min_content_light_level = {min_nits},
    }},
}}

if #edid_rates > 1 then
    entry.dynamic_refresh_rates = edid_rates
    entry.dynamic_modegen = function(base_mode, refresh)
        local m = edid_modes[refresh]
        if m == nil then
            return base_mode
        end
        local mode = base_mode
        for k, v in pairs(m) do
            mode[k] = v
        end
        return mode
    end
end

entry.matches = function(display)
    if display.vendor == {_lua_str(info["vendor"])} and display.product == {info["product"]}{model_check} then
        -- Hereda refresco/colorimetria de otro perfil del sistema para este panel, si existe
        local best, best_prio = nil, -1
        for key, other in pairs(gamescope.config.known_displays) do
            if key ~= "{ENTRY_KEY}" and type(other) == "table" and type(other.matches) == "function" then
                local ok, prio = pcall(other.matches, display)
                if ok and type(prio) == "number" and prio > best_prio then
                    best, best_prio = other, prio
                end
            end
        end
        if best ~= nil and best_prio >= 0 then
            if best.dynamic_refresh_rates and best.dynamic_modegen then
                entry.dynamic_refresh_rates = best.dynamic_refresh_rates
                entry.dynamic_modegen = best.dynamic_modegen
            end
            if best.colorimetry then
                entry.colorimetry = best.colorimetry
            end
        end
        return 9000
    end
    return -1
end

gamescope.config.known_displays.{ENTRY_KEY} = entry
debug("[AutoHDR] Panel interno {pretty} registrado como HDR10")
"""


def script_path(home: str) -> str:
    return os.path.join(home, ".config", "gamescope", "scripts", SCRIPT_NAME)


def install(home: str, lua: str) -> str:
    path = script_path(home)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(lua)
    os.replace(tmp, path)
    return path


def uninstall(home: str) -> bool:
    try:
        os.remove(script_path(home))
        return True
    except FileNotFoundError:
        return False
