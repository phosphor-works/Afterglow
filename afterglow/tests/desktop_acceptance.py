#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native AT-SPI text and selection, and OSC 1337 save and cancel acceptance.
Requires python-dbus and the nested_desktop dependencies. Runs on a private bus.
"""
import argparse
import json
import os
import shlex
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import dbus
from nested_desktop import NestedDesktop, check, wait_for

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--private-bus', action='store_true')
parser.add_argument('--it2dl', type=Path, help='Optional upstream it2dl script to exercise instead of the inline fixture')
args = parser.parse_args()
binary = str(args.binary.resolve())
if not args.private_bus:
    sys.exit(subprocess.call(['dbus-run-session', '--', sys.executable, '-u', __file__, binary, '--private-bus'] + (['--it2dl', str(args.it2dl.resolve())] if args.it2dl else [])))
REGISTRY_PATHS = ('/usr/lib/at-spi2-registryd', '/usr/libexec/at-spi2-registryd',
                  '/usr/lib/at-spi2-core/at-spi2-registryd', '/usr/libexec/at-spi2-core/at-spi2-registryd')
registryd = next((path for path in REGISTRY_PATHS if os.access(path, os.X_OK)), shutil.which('at-spi2-registryd'))
if not registryd:
    sys.exit('at-spi2-registryd was not found')
A = 'org.a11y.atspi.Accessible'
T = 'org.a11y.atspi.Text'
P = 'org.freedesktop.DBus.Properties'
with tempfile.TemporaryDirectory(prefix='afterglow-desktop-') as tmp:
    root = Path(tmp)
    config = root / 'afterglow'
    config.mkdir()
    (config / 'config.json').write_text(json.dumps({
        'Terminal': {'shell': '/bin/bash', 'env': {'HOME': tmp}, 'confirmClose': False},
        'Control': {'allowReadText': True}, 'QuickTerminal': {'shortcut': ''},
        'Animation': {'cursor': False, 'scroll': False},
    }))
    (root / '.bashrc').write_text("PS1='desktop> '\nprintf 'ACCESSIBLE-READY\\n'\n")
    # Logs outlive the temporary root. A private directory avoids collisions in a shared /tmp.
    logs = Path(tempfile.mkdtemp(prefix='afterglow-desktop-logs-'))
    print('Logs:', logs)
    def accessibility_bus():
        address = dbus.Interface(dbus.SessionBus().get_object('org.a11y.Bus', '/org/a11y/bus'), 'org.a11y.Bus').GetAddress()
        return address, dbus.bus.BusConnection(str(address))
    with NestedDesktop(root) as desktop, open(logs / 'native.log', 'w') as log:
        env = dict(desktop.env, QT_LINUX_ACCESSIBILITY_ALWAYS_ON='1', QT_ACCESSIBILITY='1', QT_QPA_PLATFORMTHEME='generic', QT_LOGGING_TO_CONSOLE='1', QT_FORCE_STDERR_LOGGING='1')
        registry_process = app = None
        def ctl(*items):
            p = subprocess.run([binary, 'ctl', *map(str, items)], env=env, capture_output=True, text=True, timeout=10)
            if p.returncode: raise RuntimeError(p.stderr)
            return p.stdout.strip()
        def panes():
            try: return json.loads(ctl('ls', '--json'))
            except RuntimeError: return []
        try:
            address, bus = accessibility_bus()
            registry_process = subprocess.Popen([registryd], env=dict(env, AT_SPI_BUS_ADDRESS=str(address)), stdout=log, stderr=subprocess.STDOUT)
            wait_for(lambda: bus.name_has_owner('org.a11y.atspi.Registry'))
            app = subprocess.Popen([binary, '--title', 'Desktop acceptance'], env=env, stdout=log, stderr=subprocess.STDOUT)
            wait_for(panes)
            desktop.activate()
            address, bus = accessibility_bus()
            registry = bus.get_object('org.a11y.atspi.Registry', '/org/a11y/atspi/accessible/root')
            def terminals():
                found = []
                def visit(ref, depth=0):
                    if depth > 5: return
                    obj = bus.get_object(str(ref[0]), str(ref[1]))
                    iface = dbus.Interface(obj, A)
                    if str(iface.GetRoleName()) == 'terminal': found.append(obj)
                    for child in iface.GetChildren(): visit(child, depth + 1)
                for child in dbus.Interface(registry, A).GetChildren(): visit(child)
                return found
            terminal = wait_for(lambda: terminals() and terminals()[0], timeout=20)
            text = dbus.Interface(terminal, T)
            wait_for(lambda: 'ACCESSIBLE-READY' in str(text.GetText(0, -1)))
            content = str(text.GetText(0, -1))
            print('AT-SPI text:', repr(content))
            start = content.index('ACCESSIBLE-READY')
            check(text.AddSelection(start, start + 10))
            check(tuple(map(int, text.GetSelection(0))) == (start, start + 10))
            check(int(text.GetNSelections()) == 1)
            check(text.RemoveSelection(0))
            cursor = int(dbus.Interface(terminal, P).Get(T, 'CaretOffset'))
            check(content[:cursor].endswith('desktop> '), (content, cursor))
            rect = text.GetCharacterExtents(start, 0)
            check(int(rect[2]) > 0 and int(rect[3]) > 0)
            check(int(text.GetOffsetAtPoint(int(rect[0]) + 1, int(rect[1]) + 1, 0)) == start)
            print('PASS: AT-SPI content, cursor, selection and character geometry')
            def nodes():
                result = []
                def visit(ref, depth=0):
                    if depth > 12: return
                    try:
                        obj = bus.get_object(str(ref[0]), str(ref[1])); accessible = dbus.Interface(obj, A)
                        name = str(dbus.Interface(obj, P).Get(A, 'Name'))
                        result.append((obj, name, str(accessible.GetRoleName()), list(accessible.GetInterfaces())))
                        for child in accessible.GetChildren(): visit(child, depth+1)
                    except dbus.exceptions.DBusException:
                        return  # Dialog children can disappear between queries.
                for ref in dbus.Interface(registry, A).GetChildren(): visit(ref)
                return result
            def button(name):
                for obj, label, role, interfaces in nodes():
                    if label.replace('&', '').lower() == name and role == 'push button': return obj
                return None
            def press(name):
                obj = wait_for(lambda: button(name))
                check(dbus.Interface(obj, 'org.a11y.atspi.Action').DoAction(0))
            # A real protocol download; receiving alone must not write a file.
            fixture = root / 'download.sh'
            offered = 'desktop-test.txt'
            fixture.write_text("printf '\\033]1337;File=name=ZGVza3RvcC10ZXN0LnR4dA==;size=5:aGVsbG8=\\007'\n")
            source = None
            if args.it2dl:
                source = root / offered
                source.write_bytes(b'hello')
                fixture.write_text(shlex.join(['bash', str(args.it2dl.resolve()), str(source)]) + '\n')
            def offer(): ctl('send-text', '--pane', panes()[0]['id'], shlex.join(['bash', str(fixture)]) + '\n')
            offer()
            press('cancel')
            wait_for(lambda: button('cancel') is None)
            # The home directory, and every download directory in it, is under root.
            written = [p for p in root.rglob(offered) if p != source]
            check(not written, written)
            check(not (root / 'accepted.txt').exists())
            time.sleep(.5)
            offer()
            wait_for(lambda: button('save'))
            destination = root / 'accepted.txt'
            fields = [(obj, name) for obj, name, role, interfaces in nodes() if 'org.a11y.atspi.EditableText' in interfaces]
            print('Save dialog fields:', [name for _, name in fields])
            field = next(obj for obj, name in fields if 'file name' in name.lower())
            check(dbus.Interface(field, 'org.a11y.atspi.EditableText').SetTextContents(str(destination)))
            press('save')
            wait_for(destination.exists)
            check(destination.read_bytes() == b'hello')
            print('PASS: OSC 1337 cancellation and explicit save')
        finally:
            if (root / 'kwin.log').exists():
                shutil.copyfile(root / 'kwin.log', logs / 'kwin.log')
            for process in (registry_process, app):
                if process is None:
                    continue
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
