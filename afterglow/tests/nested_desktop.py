# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in native UI tests in a temporary KWin desktop, using XTest for input.

Requires KWin, an X11/XWayland DISPLAY, libX11, libXtst, xprop and ImageMagick.
Only the nested compositor receives keys; never target a user's terminal.
"""
import ctypes as C
import os
from pathlib import Path
import subprocess
import time


def wait_for(condition, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = condition()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting for {condition}")


class NestedDesktop:
    def __init__(self, root):
        self.root = root
        self.socket = f"afterglow-ui-{os.getpid()}"
        self.env = dict(os.environ, XDG_CONFIG_HOME=str(root),
                        QT_QPA_PLATFORM="wayland", WAYLAND_DISPLAY=self.socket)
        (root / "kwinrc").write_text("[Plugins]\nplasmazonesEnabled=false\n")
        self.log = (root / "kwin.log").open("w")
        self.process = None
        self.display = None

    def __enter__(self):
        try:
            self.process = subprocess.Popen(
                ["kwin_wayland", "--x11-display", os.environ["DISPLAY"],
                 "--socket", self.socket, "--width", "1100", "--height", "750",
                 "--no-lockscreen", "--no-global-shortcuts", "--no-kactivities"],
                env=dict(self.env, QT_QPA_PLATFORM="xcb", WAYLAND_DISPLAY=""),
                stdout=self.log, stderr=subprocess.STDOUT)
            wait_for(lambda: (Path(os.environ["XDG_RUNTIME_DIR"]) / self.socket).exists())
            self.x = C.CDLL("libX11.so.6")
            self.xt = C.CDLL("libXtst.so.6")
            self.x.XOpenDisplay.argtypes = [C.c_char_p]
            self.x.XOpenDisplay.restype = C.c_void_p
            self.x.XCloseDisplay.argtypes = [C.c_void_p]
            self.x.XDefaultRootWindow.argtypes = [C.c_void_p]
            self.x.XDefaultRootWindow.restype = C.c_ulong
            self.x.XQueryTree.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong), C.POINTER(C.c_ulong),
                                         C.POINTER(C.POINTER(C.c_ulong)), C.POINTER(C.c_uint)]
            self.x.XFree.argtypes = [C.c_void_p]
            self.x.XRaiseWindow.argtypes = [C.c_void_p, C.c_ulong]
            self.x.XSetInputFocus.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_ulong]
            self.x.XGetInputFocus.argtypes = [C.c_void_p, C.POINTER(C.c_ulong), C.POINTER(C.c_int)]
            self.x.XFlush.argtypes = [C.c_void_p]
            self.x.XStringToKeysym.argtypes = [C.c_char_p]
            self.x.XStringToKeysym.restype = C.c_ulong
            self.x.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
            self.x.XKeysymToKeycode.restype = C.c_uint
            self.xt.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
            self.display = self.x.XOpenDisplay(None)
            assert self.display, "Cannot open DISPLAY"
            self.window = wait_for(lambda: self.find_window(self.x.XDefaultRootWindow(self.display)))
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def find_window(self, window):
        props = subprocess.run(["xprop", "-id", hex(window), "_NET_WM_PID", "_NET_WM_NAME"],
                               capture_output=True, text=True, timeout=5).stdout
        if f"_NET_WM_PID(CARDINAL) = {self.process.pid}" in props and "_NET_WM_NAME(UTF8_STRING) =" in props:
            return window
        root, parent, count = C.c_ulong(), C.c_ulong(), C.c_uint()
        children = C.POINTER(C.c_ulong)()
        if self.x.XQueryTree(self.display, window, C.byref(root), C.byref(parent), C.byref(children), C.byref(count)):
            windows = list(children[:count.value])
            self.x.XFree(children)
            for child in windows:
                found = self.find_window(child)
                if found:
                    return found

    def activate(self):
        self.x.XGetGeometry.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong),
            C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_uint), C.POINTER(C.c_uint),
            C.POINTER(C.c_uint), C.POINTER(C.c_uint)]
        root, x, y = C.c_ulong(), C.c_int(), C.c_int()
        w, h, border, depth = C.c_uint(), C.c_uint(), C.c_uint(), C.c_uint()
        assert self.x.XGetGeometry(self.display, self.window, C.byref(root), C.byref(x), C.byref(y),
            C.byref(w), C.byref(h), C.byref(border), C.byref(depth))
        width, height = w.value, h.value
        self.x.XWarpPointer.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_int, C.c_int]
        self.xt.XTestFakeButtonEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
        self.x.XRaiseWindow(self.display, self.window)
        self.x.XWarpPointer(self.display, 0, self.window, 0, 0, 0, 0, width // 2, height // 2)
        self.xt.XTestFakeButtonEvent(self.display, 1, 1, 0)
        self.xt.XTestFakeButtonEvent(self.display, 1, 0, 0)
        self.x.XFlush(self.display)
        time.sleep(0.3)

    def key(self, name, modifiers=()):
        self.x.XRaiseWindow(self.display, self.window)
        self.x.XSetInputFocus(self.display, self.window, 1, 0)
        self.x.XFlush(self.display)
        focused, revert = C.c_ulong(), C.c_int()
        self.x.XGetInputFocus(self.display, C.byref(focused), C.byref(revert))
        assert focused.value == self.window, "Nested desktop lost focus; refusing input"
        def event(key, down):
            code = self.x.XKeysymToKeycode(self.display, self.x.XStringToKeysym(key.encode()))
            assert code, key
            self.xt.XTestFakeKeyEvent(self.display, code, down, 0)
        for modifier in modifiers:
            event(modifier, 1)
        event(name, 1)
        event(name, 0)
        for modifier in reversed(modifiers):
            event(modifier, 0)
        self.x.XFlush(self.display)
        time.sleep(0.06)

    def text(self, value):
        for char in value:
            self.key({" ": "space", "-": "minus", "/": "slash", ".": "period"}.get(char, char))

    def screenshot(self, path):
        subprocess.run(["import", "-window", hex(self.window), str(path)], check=True, timeout=10)

    def __exit__(self, *_):
        if self.display:
            self.x.XCloseDisplay(self.display)
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()
