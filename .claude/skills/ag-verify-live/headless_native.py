#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run afterglow_native_tests in a headless nested KWin, with no window on the user's desktop.

Usage:
  python3 .claude/skills/ag-verify-live/headless_native.py [--filter 'NativeWindow.*'] [--icd lvp.json]

--icd selects a Vulkan driver manifest, such as a lavapipe build for software rendering. Without it the tests use
the host driver. On a failure, the last KWin log lines are printed. The runner starts itself again under
dbus-run-session, so that KWin and the tests use a private session bus.
"""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'afterglow/tests'))
from nested_desktop import KWINRC  # noqa: E402  (one KWin configuration for every runner)

parser = argparse.ArgumentParser()
parser.add_argument('--binary', default='build/afterglow/afterglow_native_tests')
parser.add_argument('--filter', default='NativeWindow.*', help='--gtest_filter pattern')
parser.add_argument('--icd', help='Vulkan ICD manifest (for example a lavapipe lvp.json)')
parser.add_argument('--timeout', type=int, default=400, help='seconds for the whole test binary')
parser.add_argument('--size', default='1100x750', help='virtual output size')
parser.add_argument('--validation', action='store_true', help='enable the Vulkan validation layer')
parser.add_argument('--private-bus', action='store_true', help=argparse.SUPPRESS)
args = parser.parse_args()

binary = Path(args.binary).resolve()
if not binary.exists():
    sys.exit(f'{binary} does not exist. Build it: cmake --build build --target afterglow_native_tests')
if not args.private_bus:
    # KWin must not join the user's session bus: run everything below on a private one.
    sys.exit(subprocess.call(['dbus-run-session', '--', sys.executable, __file__, *sys.argv[1:],
                              '--binary', str(binary), '--private-bus']))
# Stop KWin on SIGTERM too, through the finally block below.
signal.signal(signal.SIGTERM, lambda *_: sys.exit(128 + signal.SIGTERM))
width, height = args.size.split('x')

with tempfile.TemporaryDirectory(prefix='afterglow-headless-') as temporary:
    root = Path(temporary)
    config = root / 'config'
    config.mkdir()
    # The user's KWin plugins (PlasmaZones) must not load into the test compositor
    (config / 'kwinrc').write_text(KWINRC)
    socket = f'afterglow-headless-{os.getpid()}'
    env = dict(
        os.environ,
        XDG_CONFIG_HOME=str(config),
        # Scripts, packs and state under the user's own data directory must not reach the tests.
        XDG_DATA_HOME=str(root / 'data'),
        QT_FORCE_STDERR_LOGGING='1',
        QT_QPA_PLATFORM='offscreen',
        WAYLAND_DISPLAY='',
        QT_QUICK_CONTROLS_STYLE='Basic',
        QT_QPA_PLATFORMTHEME='generic',
    )
    if args.icd:
        env.update(
            VK_DRIVER_FILES=str(Path(args.icd).resolve()),
            KWIN_COMPOSE='O2',
            __EGL_VENDOR_LIBRARY_FILENAMES='/usr/share/glvnd/egl_vendor.d/50_mesa.json',
            GALLIUM_DRIVER='llvmpipe',
            LIBGL_ALWAYS_SOFTWARE='1',
        )
    log_path = root / 'kwin.log'
    with log_path.open('w') as log:
        compositor = subprocess.Popen(
            ['kwin_wayland', '--virtual', '--socket', socket, '--width', width, '--height', height,
             '--no-lockscreen', '--no-global-shortcuts', '--no-kactivities'],
            env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while not (Path(env['XDG_RUNTIME_DIR']) / socket).exists():
                if compositor.poll() is not None:
                    sys.exit('KWin exited at startup:\n' + log_path.read_text()[-2000:])
                if time.monotonic() > deadline:
                    sys.exit('KWin did not create its socket in 10 s')
                time.sleep(0.05)
            # The binary refuses to run outside a nested desktop with its own bus.
            env.update(QT_QPA_PLATFORM='wayland', WAYLAND_DISPLAY=socket, AFTERGLOW_NATIVE_ISOLATED='1')
            if args.validation:
                env.update(AFTERGLOW_VULKAN_VALIDATION='1', QT_LOGGING_RULES='qt.vulkan=true;afterglow.gpu=true')
            result = subprocess.run(
                ['dbus-run-session', '--', str(binary), f'--gtest_filter={args.filter}'],
                env=env, cwd=root, timeout=args.timeout + 10)
            code = result.returncode
        except subprocess.TimeoutExpired:
            print(f'Timed out after {args.timeout} s', file=sys.stderr)
            code = 124
        finally:
            compositor.terminate()
            try:
                compositor.wait(timeout=5)
            except subprocess.TimeoutExpired:
                compositor.kill()
                compositor.wait()
    if code != 0:
        print('--- last KWin log lines ---', file=sys.stderr)
        print('\n'.join(log_path.read_text().splitlines()[-30:]), file=sys.stderr)
    sys.exit(code)
