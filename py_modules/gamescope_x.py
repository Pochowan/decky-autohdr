"""
Escritura de propiedades X en la ventana raíz del Xwayland de gamescope.

gamescope (steamcompmgr) observa propiedades como GAMESCOPE_HDR_ITM_ENABLE
en la raíz del Xwayland donde corre Steam. Es el mismo mecanismo que usa
el propio cliente de Steam para su configuración de pantalla, así que no
se toca el proceso del juego.

Usa libX11 vía ctypes (sin dependencias). Si falla, recurre a `xprop`.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import subprocess

XA_CARDINAL = 6
XA_STRING = 31
PROP_MODE_REPLACE = 0


def _find_session_env() -> dict:
    """Busca DISPLAY/XAUTHORITY en el entorno del proceso de Steam."""
    wanted = ("steam", "steamwebhelper", "gamescope-wl", "gamescope")
    found: dict = {}
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/comm", "r") as f:
                comm = f.read().strip()
            if comm not in wanted:
                continue
            with open(f"/proc/{pid}/environ", "rb") as f:
                raw = f.read().split(b"\0")
        except OSError:
            continue
        env = {}
        for item in raw:
            if b"=" in item:
                k, v = item.split(b"=", 1)
                env[k.decode(errors="ignore")] = v.decode(errors="ignore")
        if env.get("DISPLAY"):
            found = {"DISPLAY": env["DISPLAY"]}
            if env.get("XAUTHORITY"):
                found["XAUTHORITY"] = env["XAUTHORITY"]
            if comm == "steam":  # preferido: es el Xwayland raíz de gamescope
                return found
    return found


class GamescopeX:
    def __init__(self, logger=None):
        self.log = logger
        self._xlib = None
        try:
            name = ctypes.util.find_library("X11") or "libX11.so.6"
            x = ctypes.CDLL(name)
            x.XOpenDisplay.restype = ctypes.c_void_p
            x.XOpenDisplay.argtypes = [ctypes.c_char_p]
            x.XDefaultRootWindow.restype = ctypes.c_ulong
            x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
            x.XInternAtom.restype = ctypes.c_ulong
            x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
            x.XChangeProperty.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
                ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
            ]
            x.XDeleteProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong]
            x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
            x.XCloseDisplay.argtypes = [ctypes.c_void_p]
            x.XGetWindowProperty.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long,
                ctypes.c_int, ctypes.c_ulong,
                ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                ctypes.POINTER(ctypes.c_void_p),
            ]
            x.XFree.argtypes = [ctypes.c_void_p]
            self._xlib = x
        except OSError as e:
            self._warn(f"libX11 no disponible, se usará xprop: {e}")

    def _warn(self, msg):
        if self.log:
            self.log.warning(msg)

    def env(self) -> dict:
        e = _find_session_env()
        if not e.get("DISPLAY"):
            e["DISPLAY"] = os.environ.get("DISPLAY") or ":0"
        return e

    # ---- API pública -------------------------------------------------------

    def apply(self, cardinals: dict, strings: dict, deletes: list) -> str:
        """Aplica en una sola conexión. Devuelve el DISPLAY usado."""
        e = self.env()
        if self._xlib is not None:
            try:
                self._apply_xlib(e, cardinals, strings, deletes)
                return e["DISPLAY"]
            except Exception as ex:  # noqa: BLE001
                self._warn(f"Fallo vía libX11 ({ex}); probando xprop")
        self._apply_xprop(e, cardinals, strings, deletes)
        return e["DISPLAY"]

    def read_cardinals(self, names: list) -> dict:
        """Lee propiedades CARDINAL de la raíz. Valor None si no existe/falla."""
        out = {n: None for n in names}
        if self._xlib is None:
            return out
        x = self._xlib
        try:
            dpy = self._open(self.env())
        except Exception:  # noqa: BLE001
            return out
        try:
            root = x.XDefaultRootWindow(dpy)
            for n in names:
                atom = x.XInternAtom(dpy, n.encode(), 0)
                at, af = ctypes.c_ulong(), ctypes.c_int()
                ni, ba = ctypes.c_ulong(), ctypes.c_ulong()
                prop = ctypes.c_void_p()
                rc = x.XGetWindowProperty(dpy, root, atom, 0, 1, 0, 0,
                                          ctypes.byref(at), ctypes.byref(af), ctypes.byref(ni),
                                          ctypes.byref(ba), ctypes.byref(prop))
                if rc == 0 and prop.value:
                    if af.value == 32 and ni.value >= 1:
                        out[n] = ctypes.cast(prop, ctypes.POINTER(ctypes.c_ulong))[0] & 0xFFFFFFFF
                    x.XFree(prop)
        finally:
            x.XCloseDisplay(dpy)
        return out

    # ---- Implementaciones --------------------------------------------------

    def _open(self, e):
        x = self._xlib
        old = os.environ.get("XAUTHORITY")
        if e.get("XAUTHORITY"):
            os.environ["XAUTHORITY"] = e["XAUTHORITY"]
        try:
            dpy = x.XOpenDisplay(e["DISPLAY"].encode())
        finally:
            if e.get("XAUTHORITY"):
                if old is None:
                    os.environ.pop("XAUTHORITY", None)
                else:
                    os.environ["XAUTHORITY"] = old
        if not dpy:
            raise RuntimeError(f"no se pudo abrir {e['DISPLAY']}")
        return dpy

    def _apply_xlib(self, e, cardinals, strings, deletes):
        x = self._xlib
        dpy = self._open(e)
        try:
            root = x.XDefaultRootWindow(dpy)
            for name, val in cardinals.items():
                atom = x.XInternAtom(dpy, name.encode(), 0)
                # formato 32 => array de C long (64 bits en x86_64)
                data = (ctypes.c_ulong * 1)(int(val) & 0xFFFFFFFF)
                x.XChangeProperty(dpy, root, atom, XA_CARDINAL, 32, PROP_MODE_REPLACE, data, 1)
            for name, val in strings.items():
                atom = x.XInternAtom(dpy, name.encode(), 0)
                raw = val.encode()
                buf = ctypes.create_string_buffer(raw, len(raw))
                x.XChangeProperty(dpy, root, atom, XA_STRING, 8, PROP_MODE_REPLACE, buf, len(raw))
            for name in deletes:
                atom = x.XInternAtom(dpy, name.encode(), 0)
                x.XDeleteProperty(dpy, root, atom)
            x.XSync(dpy, 0)
        finally:
            x.XCloseDisplay(dpy)

    def _apply_xprop(self, e, cardinals, strings, deletes):
        env = dict(os.environ)
        env.pop("LD_LIBRARY_PATH", None)  # el Python de Decky (PyInstaller) lo altera
        env.pop("LD_PRELOAD", None)
        env.update(e)
        base = ["xprop", "-root", "-display", e["DISPLAY"]]
        cmds = []
        for name, val in cardinals.items():
            cmds.append(base + ["-f", name, "32c", "-set", name, str(int(val))])
        for name, val in strings.items():
            cmds.append(base + ["-f", name, "8s", "-set", name, val])
        for c in cmds:
            subprocess.run(c, env=env, check=True, capture_output=True, timeout=5)
        for name in deletes:
            subprocess.run(base + ["-remove", name], env=env, check=False, capture_output=True, timeout=5)
