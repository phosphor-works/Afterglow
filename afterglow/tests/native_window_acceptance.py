#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run real TerminalWindow integration tests in an isolated nested compositor."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from nested_desktop import NestedDesktop, wait_for

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True)
parser.add_argument("--filter", help="gtest filter, for example 'NativeWindow.KittyDragAndDropClient'")
# Below the CTest TIMEOUT, so that the runner reports a stall before CTest kills it.
parser.add_argument("--timeout", type=int, default=110, help="seconds for the whole run")
parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
binary = os.path.abspath(args.binary)
if not args.private_bus:
    # The nested KWin must not join the user's session bus.
    command = [sys.executable, __file__, "--binary", binary, "--timeout", str(args.timeout), "--private-bus"]
    if args.filter:
        command += ["--filter", args.filter]
    sys.exit(subprocess.call(["dbus-run-session", "--", *command]))
deadline = time.monotonic() + args.timeout
# Run the clean-up below on SIGTERM too: the binary is in its own session.
signal.signal(signal.SIGTERM, lambda *_: sys.exit(128 + signal.SIGTERM))
with tempfile.TemporaryDirectory(prefix="afterglow-native-") as temporary:
    root = Path(temporary)
    with NestedDesktop(root) as desktop:
        marker = root / "drag-ready"
        env = dict(desktop.env, AFTERGLOW_NATIVE_DND_READY=str(marker), AFTERGLOW_NATIVE_ISOLATED="1")
        command = ["dbus-run-session", "--", binary]
        if args.filter:
            command.append("--gtest_filter=" + args.filter)
        # Run in the temporary root: kitten leaves its .dnd-kitten-drop-* directories
        # in the working directory. A new session lets the runner stop the binary and
        # its private dbus-daemon together; dbus-run-session does not pass SIGTERM on.
        process = subprocess.Popen(command, env=env, cwd=root, start_new_session=True)
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
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError(f"The native tests did not finish within {args.timeout} s")
                wait_for(ready, timeout=remaining)
                if len(coordinates) == 4:
                    marker.unlink(missing_ok=True)
                    Path(str(marker) + ".pressed").unlink(missing_ok=True)
                    start_x, start_y, end_x, end_y = coordinates
                    # Feed only the nested compositor's X11 backend. KWin then
                    # generates real Wayland pointer serials and negotiates QDrag.
                    # Host XWayland may suppress XTest button events during grabs.
                    desktop.pointer(6, start_x, start_y)
                    time.sleep(.1)
                    desktop.pointer(4, start_x, start_y)
                    try:
                        wait_for(lambda: Path(str(marker) + ".pressed").exists(), timeout=5)
                        for step in range(1, 11):
                            desktop.pointer(6, start_x + (end_x - start_x) * step // 10,
                                            start_y + (end_y - start_y) * step // 10, held=True)
                            time.sleep(.04)
                    finally:
                        desktop.pointer(5, end_x, end_y, held=True)
            raise SystemExit(process.wait(timeout=30))
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
