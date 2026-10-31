#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run real TerminalWindow integration tests in an isolated nested compositor."""
import argparse
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import time
from nested_desktop import NestedDesktop, wait_for

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="afterglow-native-") as temporary:
    root = Path(temporary)
    with NestedDesktop(root) as desktop:
        marker = root / "drag-ready"
        # Keep desktop portal activation outside this isolated input test.
        env = dict(desktop.env, AFTERGLOW_NATIVE_DND_READY=str(marker),
                   QT_QUICK_CONTROLS_STYLE="Basic", QT_QPA_PLATFORMTHEME="generic")
        process = subprocess.Popen(["dbus-run-session", "--", os.path.abspath(args.binary)], env=env)
        try:
            while process.poll() is None:
                coordinates = []
                def ready():
                    if process.poll() is not None:
                        return True
                    try:
                        coordinates[:] = map(int, marker.read_text().split())
                    except (FileNotFoundError, ValueError):
                        return False
                    return len(coordinates) == 4
                wait_for(ready, timeout=60)
                if len(coordinates) == 4:
                    marker.unlink(missing_ok=True)
                    Path(str(marker) + ".pressed").unlink(missing_ok=True)
                    x, display = desktop.x, desktop.display
                    start_x, start_y, end_x, end_y = coordinates
                    # Feed only the nested compositor's X11 backend. KWin then
                    # generates real Wayland pointer serials and negotiates QDrag.
                    # Host XWayland may suppress XTest button events during grabs.
                    class ButtonEvent(C.Structure):
                        _fields_ = [("type", C.c_int), ("serial", C.c_ulong), ("send_event", C.c_int),
                                    ("display", C.c_void_p), ("window", C.c_ulong), ("root", C.c_ulong),
                                    ("subwindow", C.c_ulong), ("time", C.c_ulong), ("x", C.c_int),
                                    ("y", C.c_int), ("x_root", C.c_int), ("y_root", C.c_int),
                                    ("state", C.c_uint), ("button", C.c_uint), ("same_screen", C.c_int)]
                    class MotionEvent(C.Structure):
                        _fields_ = ButtonEvent._fields_[:-2] + [("is_hint", C.c_char), ("same_screen", C.c_int)]
                    class XEvent(C.Union):
                        _fields_ = [("button", ButtonEvent), ("motion", MotionEvent), ("padding", C.c_long * 24)]
                    x.XSendEvent.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_long, C.POINTER(XEvent)]
                    x.XTranslateCoordinates.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong, C.c_int, C.c_int,
                                                        C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_ulong)]
                    def send(kind, px, py, held):
                        event = XEvent()
                        value = event.motion if kind == 6 else event.button
                        value.type, value.display, value.window = kind, display, desktop.window
                        value.root = x.XDefaultRootWindow(display)
                        value.time = int(time.monotonic() * 1000) & 0xffffffff
                        value.x, value.y = px, py
                        rx, ry, child = C.c_int(), C.c_int(), C.c_ulong()
                        assert x.XTranslateCoordinates(display, desktop.window, value.root, px, py,
                                                       C.byref(rx), C.byref(ry), C.byref(child))
                        value.x_root, value.y_root = rx.value, ry.value
                        value.state, value.same_screen = (256 if held else 0), 1
                        if kind != 6:
                            value.button = 1
                        mask = (1 << 6) | (1 << 13) if kind == 6 else (1 << (kind - 2))
                        assert x.XSendEvent(display, desktop.window, 0, mask, C.byref(event))
                        x.XFlush(display)
                    send(6, start_x, start_y, False)
                    time.sleep(.1)
                    send(4, start_x, start_y, False)
                    try:
                        wait_for(lambda: Path(str(marker) + ".pressed").exists(), timeout=5)
                        for step in range(1, 11):
                            send(6, start_x + (end_x - start_x) * step // 10,
                                 start_y + (end_y - start_y) * step // 10, True)
                            time.sleep(.04)
                    finally:
                        send(5, end_x, end_y, True)
            raise SystemExit(process.wait(timeout=30))
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
