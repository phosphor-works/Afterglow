# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in native UI tests in a temporary KWin desktop.

Requires KWin, an X11/XWayland DISPLAY, libX11 and xprop, and ImageMagick for
screenshots. Run the caller under dbus-run-session, so that the nested KWin does
not join the user's session bus. Pointer and key input goes to the nested KWin
window as synthetic X events (XSendEvent), never through XTest, so it cannot
reach another host window such as the user's terminal.
"""
import ctypes as C
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

# The user's KWin plugins (PlasmaZones) must not load into the test compositor.
KWINRC = ("[Plugins]\nplasmazonesEnabled=false\nkwin_effect_plasmazonesEnabled=false\n"
          "kwin_effect_plasmazones_overviewEnabled=false\n")

# Key names for characters that XStringToKeysym does not take as they are.
KEY_NAMES = {" ": "space", "-": "minus", "/": "slash", ".": "period", ",": "comma", "_": "underscore",
             "=": "equal", "+": "plus", ":": "colon", ";": "semicolon", "'": "apostrophe",
             '"': "quotedbl", "!": "exclam", "?": "question", "(": "parenleft", ")": "parenright",
             "[": "bracketleft", "]": "bracketright", "<": "less", ">": "greater", "|": "bar",
             "&": "ampersand", "*": "asterisk", "$": "dollar", "#": "numbersign", "@": "at",
             "%": "percent", "^": "asciicircum", "~": "asciitilde", "{": "braceleft", "}": "braceright"}
SHIFTED = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ_+:"!?()<>|&*$#@%^~{}')
# X modifier state bits: Shift, Control, Mod1 (Alt) and Mod4 (Super).
MODIFIER_MASKS = {"Shift": 1, "Control": 4, "Alt": 8, "Meta": 8, "Super": 64}


def check(condition, *detail):
    """Raises AssertionError when condition is false. Not assert: python -O must
    not turn the checks off, or skip the X calls that some checks make."""
    if not condition:
        raise AssertionError(*detail)


def wait_for(condition, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = condition()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting for {condition}")


class _KeyEvent(C.Structure):
    _fields_ = [("type", C.c_int), ("serial", C.c_ulong), ("send_event", C.c_int),
                ("display", C.c_void_p), ("window", C.c_ulong), ("root", C.c_ulong),
                ("subwindow", C.c_ulong), ("time", C.c_ulong), ("x", C.c_int), ("y", C.c_int),
                ("x_root", C.c_int), ("y_root", C.c_int), ("state", C.c_uint),
                ("keycode", C.c_uint), ("same_screen", C.c_int)]


class _ButtonEvent(C.Structure):
    _fields_ = _KeyEvent._fields_[:-2] + [("button", C.c_uint), ("same_screen", C.c_int)]


class _MotionEvent(C.Structure):
    _fields_ = _KeyEvent._fields_[:-2] + [("is_hint", C.c_char), ("same_screen", C.c_int)]


class _XEvent(C.Union):
    _fields_ = [("key", _KeyEvent), ("button", _ButtonEvent), ("motion", _MotionEvent),
                ("padding", C.c_long * 24)]


_ERROR_HANDLER = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p)
# Xlib's default handler calls exit() on BadWindow, for example when a host
# tooltip goes away during find_window. That skips __exit__ and leaves the nested
# KWin running. Module scope keeps the callback alive while Xlib holds it.
_IGNORE_X_ERRORS = _ERROR_HANDLER(lambda display, event: 0)


class NestedDesktop:
    def __init__(self, root, width=1100, height=750, fixed_size=False):
        if not 640 <= width <= 7680 or not 480 <= height <= 4320:
            raise ValueError("Invalid nested desktop dimensions")
        self.width, self.height = width, height
        self.fixed_size = fixed_size
        self.root = root
        self.socket = f"afterglow-ui-{os.getpid()}"
        # The application environment. A private data home keeps the user's own
        # scripts and shader packs out of the run. The generic platform theme and
        # the Basic style keep portal activation and the desktop style out of it.
        # Qt's default message pattern keeps log lines in the form that the
        # harnesses match, whatever the developer's own pattern is.
        self.env = dict(os.environ, XDG_CONFIG_HOME=str(root), XDG_DATA_HOME=str(root / "data"),
                        QT_QPA_PLATFORM="wayland", WAYLAND_DISPLAY=self.socket,
                        QT_QPA_PLATFORMTHEME="generic", QT_QUICK_CONTROLS_STYLE="Basic",
                        QT_MESSAGE_PATTERN="%{if-category}%{category}: %{endif}%{message}")
        # A harness started in an Afterglow pane inherits that pane's bus name and
        # pane ID. `afterglow ctl` would then call the user's instance (a unique
        # name of the session bus, unknown on the private bus) instead of the
        # nested one, and default to a pane that the nested one does not have.
        for name in ("AFTERGLOW_BUS_NAME", "AFTERGLOW_PANE_ID"):
            self.env.pop(name, None)
        (root / "kwinrc").write_text(KWINRC)
        self.log = (root / "kwin.log").open("w")
        self.process = None
        self.display = None

    def __enter__(self):
        try:
            kwin_env = dict(self.env, QT_QPA_PLATFORM="xcb", WAYLAND_DISPLAY="")
            for name in ("QT_QPA_PLATFORMTHEME", "QT_QUICK_CONTROLS_STYLE"):
                if name in os.environ:
                    kwin_env[name] = os.environ[name]
                else:
                    kwin_env.pop(name)
            self.process = subprocess.Popen(
                ["kwin_wayland", "--x11-display", os.environ["DISPLAY"],
                 "--socket", self.socket, "--width", str(self.width), "--height", str(self.height),
                 "--no-lockscreen", "--no-global-shortcuts", "--no-kactivities"],
                env=kwin_env, stdout=self.log, stderr=subprocess.STDOUT)
            wait_for(self._socket_ready)
            self.x = C.CDLL("libX11.so.6")
            self.x.XOpenDisplay.argtypes = [C.c_char_p]
            self.x.XOpenDisplay.restype = C.c_void_p
            self.x.XCloseDisplay.argtypes = [C.c_void_p]
            self.x.XSetErrorHandler.argtypes = [_ERROR_HANDLER]
            self.x.XSetErrorHandler.restype = C.c_void_p
            self.x.XDefaultRootWindow.argtypes = [C.c_void_p]
            self.x.XDefaultRootWindow.restype = C.c_ulong
            self.x.XQueryTree.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong), C.POINTER(C.c_ulong),
                                         C.POINTER(C.POINTER(C.c_ulong)), C.POINTER(C.c_uint)]
            self.x.XFree.argtypes = [C.c_void_p]
            self.x.XGetGeometry.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong),
                C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_uint), C.POINTER(C.c_uint),
                C.POINTER(C.c_uint), C.POINTER(C.c_uint)]
            self.x.XTranslateCoordinates.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong, C.c_int, C.c_int,
                                                     C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_ulong)]
            self.x.XSendEvent.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_long, C.POINTER(_XEvent)]
            self.x.XFlush.argtypes = [C.c_void_p]
            self.x.XStringToKeysym.argtypes = [C.c_char_p]
            self.x.XStringToKeysym.restype = C.c_ulong
            self.x.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
            self.x.XKeysymToKeycode.restype = C.c_uint
            self.x.XSetErrorHandler(_IGNORE_X_ERRORS)
            self.display = self.x.XOpenDisplay(None)
            check(self.display, "Cannot open DISPLAY")
            self.window = wait_for(lambda: self.find_window(self.x.XDefaultRootWindow(self.display)))
            if self.fixed_size:
                self.set_fixed_size()
            return self
        except BaseException:
            try:
                self.log.flush()
                tail = (self.root / "kwin.log").read_text(errors="replace")[-3000:]
                print(f"kwin_wayland log:\n{tail}", file=sys.stderr)
            except OSError:
                pass
            self.__exit__(None, None, None)
            raise

    def _socket_ready(self):
        if self.process.poll() is not None:
            raise RuntimeError(f"kwin_wayland exited with status {self.process.returncode}")
        return (Path(os.environ["XDG_RUNTIME_DIR"]) / self.socket).exists()

    def set_fixed_size(self):
        # Constrain only this temporary host window, including under host tiling.
        class SizeHints(C.Structure):
            _fields_ = [("flags", C.c_long)] + [(name, C.c_int) for name in (
                "x", "y", "width", "height", "min_width", "min_height", "max_width", "max_height",
                "width_inc", "height_inc", "min_aspect_x", "min_aspect_y", "max_aspect_x",
                "max_aspect_y", "base_width", "base_height", "win_gravity")]
        hints = SizeHints()
        hints.flags = (1 << 4) | (1 << 5)
        hints.min_width = hints.max_width = self.width
        hints.min_height = hints.max_height = self.height
        self.x.XSetWMNormalHints.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(SizeHints)]
        self.x.XResizeWindow.argtypes = [C.c_void_p, C.c_ulong, C.c_uint, C.c_uint]
        self.x.XSetWMNormalHints(self.display, self.window, C.byref(hints))
        self.x.XResizeWindow(self.display, self.window, self.width, self.height)
        self.x.XFlush(self.display)
        time.sleep(.5)

    def find_window(self, window):
        props = subprocess.run(["xprop", "-id", hex(window), "_NET_WM_PID", "_NET_WM_NAME"],
                               capture_output=True, text=True, timeout=5).stdout
        if f"_NET_WM_PID(CARDINAL) = {self.process.pid}" in props and "_NET_WM_NAME(UTF8_STRING) =" in props:
            return window
        root, parent, count = C.c_ulong(), C.c_ulong(), C.c_uint()
        children = C.POINTER(C.c_ulong)()
        # A window destroyed during the walk fails here and counts as having no children.
        if self.x.XQueryTree(self.display, window, C.byref(root), C.byref(parent), C.byref(children), C.byref(count)):
            windows = list(children[:count.value])
            if children:
                self.x.XFree(children)
            for child in windows:
                found = self.find_window(child)
                if found:
                    return found
        return None

    def _event(self, kind, px, py, state):
        event = _XEvent()
        value = event.motion if kind == 6 else event.key if kind in (2, 3) else event.button
        value.type, value.display, value.window = kind, self.display, self.window
        value.root = self.x.XDefaultRootWindow(self.display)
        value.time = int(time.monotonic() * 1000) & 0xffffffff
        value.x, value.y = px, py
        rx, ry, child = C.c_int(), C.c_int(), C.c_ulong()
        check(self.x.XTranslateCoordinates(self.display, self.window, value.root, px, py,
                                           C.byref(rx), C.byref(ry), C.byref(child)), "XTranslateCoordinates failed")
        value.x_root, value.y_root = rx.value, ry.value
        value.state, value.same_screen = state, 1
        return event, value

    def pointer(self, kind, px, py, held=False):
        """Sends a motion (6), press (4) or release (5) of button 1 to the nested KWin window."""
        event, value = self._event(kind, px, py, 256 if held else 0)
        if kind != 6:
            value.button = 1
        mask = (1 << 6) | (1 << 13) if kind == 6 else (1 << (kind - 2))
        check(self.x.XSendEvent(self.display, self.window, 0, mask, C.byref(event)), "XSendEvent failed")
        self.x.XFlush(self.display)

    def size(self):
        root, x, y = C.c_ulong(), C.c_int(), C.c_int()
        w, h, border, depth = C.c_uint(), C.c_uint(), C.c_uint(), C.c_uint()
        check(self.x.XGetGeometry(self.display, self.window, C.byref(root), C.byref(x), C.byref(y),
              C.byref(w), C.byref(h), C.byref(border), C.byref(depth)), "XGetGeometry failed")
        return w.value, h.value

    def activate(self):
        """Clicks the middle of the nested desktop, which focuses the window there."""
        width, height = self.size()
        self.pointer(6, width // 2, height // 2)
        self.pointer(4, width // 2, height // 2)
        self.pointer(5, width // 2, height // 2, held=True)
        time.sleep(0.3)

    def _keycode(self, name):
        code = self.x.XKeysymToKeycode(self.display, self.x.XStringToKeysym(name.encode()))
        check(code, f"No keycode for {name}")
        return code

    def key(self, name, modifiers=()):
        """Presses a key, named as an X keysym, with modifier keys held."""
        state = 0
        def send(code, down):
            event, value = self._event(2 if down else 3, 0, 0, state)
            value.keycode = code
            check(self.x.XSendEvent(self.display, self.window, 0, 1 if down else 2, C.byref(event)), "XSendEvent failed")
        modifier_codes = [self._keycode(modifier) for modifier in modifiers]
        for code in modifier_codes:
            send(code, True)
        for modifier in modifiers:
            state |= MODIFIER_MASKS.get(modifier.split("_")[0], 0)
        code = self._keycode(name)
        send(code, True)
        send(code, False)
        for code in reversed(modifier_codes):
            send(code, False)
        self.x.XFlush(self.display)
        time.sleep(0.06)

    def text(self, value):
        """Types ASCII text. Shift is held for capitals and shifted symbols."""
        for char in value:
            name = KEY_NAMES.get(char, char)
            self.key(name, ("Shift_L",) if char in SHIFTED else ())

    def screenshot(self, path):
        subprocess.run(["import", "-window", hex(self.window), str(path)], check=True, timeout=10)

    def __exit__(self, *_):
        if self.display:
            self.x.XCloseDisplay(self.display)
            self.display = None
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()


class Session:
    """One Afterglow application in a nested desktop, driven through `afterglow ctl`."""

    def __init__(self, binary, env, log_path):
        self.binary, self.env, self.log_path = binary, env, log_path
        self.log = log_path.open("w")
        self.app = None

    def start(self, title):
        self.app = subprocess.Popen([self.binary, "--title", title], env=self.env,
                                    stdout=self.log, stderr=subprocess.STDOUT)

    def stop(self):
        if self.app and self.app.poll() is None:
            self.app.terminate()
            try:
                self.app.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.app.kill()
                self.app.wait()

    def close(self):
        self.stop()
        self.log.close()

    def log_text(self, size=3000):
        self.log.flush()
        return self.log_path.read_text(errors="replace")[-size:]

    def ctl(self, *args):
        return subprocess.check_output([self.binary, "ctl", *map(str, args)], env=self.env,
                                       stderr=subprocess.PIPE, text=True, timeout=5).strip()

    def failing_ctl(self, *args):
        """Runs a ctl command that must fail, and returns its error text."""
        result = subprocess.run([self.binary, "ctl", *map(str, args)], env=self.env,
                                capture_output=True, text=True, timeout=5)
        check(result.returncode != 0, f"ctl {args} succeeded: {result.stdout}")
        return result.stderr

    def panes(self):
        return json.loads(self.ctl("ls", "--json"))

    def screen(self, pane):
        return self.ctl("get-text", "--pane", pane, "--screen")

    def script(self, script_id):
        """The ctl diagnostics entry of one script, or None when it is not loaded."""
        return next((s for s in json.loads(self.ctl("scripts")) if s["id"] == script_id), None)

    def wait(self, condition, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                result = condition()
                if result:
                    return result
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, json.JSONDecodeError):
                pass
            if self.app.poll() is not None:
                raise AssertionError("Afterglow exited:\n" + self.log_text())
            time.sleep(0.05)
        raise AssertionError("Timeout:\n" + self.log_text(2000))

    def identity(self, pane):
        """The shell PID of a pane, from a fresh probe so that old output cannot answer."""
        nonce = uuid.uuid4().hex[:12]
        self.ctl("send-text", "--pane", pane, f"printf 'PID_{nonce}_%s_END\\n' $$\\n")
        return self.wait(lambda: re.findall(rf"PID_{nonce}_(\d+)_END", self.screen(pane)))[-1]
