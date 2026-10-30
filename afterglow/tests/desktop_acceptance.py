#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""D1 native AT-SPI text/selection and OSC 1337 save/cancel acceptance.
Requires python-dbus and the nested_desktop dependencies. Runs on a private bus.
"""
import argparse
import json
import os
import shlex
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import dbus
from nested_desktop import NestedDesktop, wait_for

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--private-bus', action='store_true')
parser.add_argument('--it2dl', type=Path, help='Optional upstream it2dl script to exercise instead of the inline fixture')
args = parser.parse_args()
binary = str(args.binary.resolve())
if not args.private_bus:
    sys.exit(subprocess.call(['dbus-run-session', '--', sys.executable, '-u', __file__, binary, '--private-bus'] + (['--it2dl', str(args.it2dl.resolve())] if args.it2dl else [])))
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
    (root / '.bashrc').write_text("PS1='D1> '\nprintf 'ACCESSIBLE-READY\\n'\n")
    with NestedDesktop(root) as desktop, open('/tmp/afterglow-d1-native.log', 'w') as log:
        env = dict(desktop.env, QT_LINUX_ACCESSIBILITY_ALWAYS_ON='1', QT_ACCESSIBILITY='1', QT_QPA_PLATFORMTHEME='generic', QT_LOGGING_TO_CONSOLE='1', QT_FORCE_STDERR_LOGGING='1')
        session = dbus.SessionBus()
        address = dbus.Interface(session.get_object('org.a11y.Bus', '/org/a11y/bus'), 'org.a11y.Bus').GetAddress()
        bus = dbus.bus.BusConnection(str(address))
        registry_process = subprocess.Popen(['/usr/lib/at-spi2-registryd'], env=dict(env, AT_SPI_BUS_ADDRESS=str(address)), stdout=log, stderr=subprocess.STDOUT)
        wait_for(lambda: bus.name_has_owner('org.a11y.atspi.Registry'))
        app = subprocess.Popen([binary, '--title', 'D1 native acceptance'], env=env, stdout=log, stderr=subprocess.STDOUT)
        def ctl(*items):
            p = subprocess.run([binary, 'ctl', *map(str, items)], env=env, capture_output=True, text=True, timeout=10)
            if p.returncode: raise RuntimeError(p.stderr)
            return p.stdout.strip()
        def panes():
            try: return json.loads(ctl('ls', '--json'))
            except RuntimeError: return []
        try:
            wait_for(panes)
            desktop.activate()
            session = dbus.SessionBus()
            address = dbus.Interface(session.get_object('org.a11y.Bus', '/org/a11y/bus'), 'org.a11y.Bus').GetAddress()
            bus = dbus.bus.BusConnection(str(address))
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
            assert text.AddSelection(start, start + 10)
            assert tuple(map(int, text.GetSelection(0))) == (start, start + 10)
            assert int(text.GetNSelections()) == 1
            assert text.RemoveSelection(0)
            cursor = int(dbus.Interface(terminal, P).Get(T, 'CaretOffset'))
            assert content[:cursor].endswith('D1> '), (content, cursor)
            rect = text.GetCharacterExtents(start, 0)
            assert int(rect[2]) > 0 and int(rect[3]) > 0
            assert int(text.GetOffsetAtPoint(int(rect[0]) + 1, int(rect[1]) + 1, 0)) == start
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
                assert dbus.Interface(obj, 'org.a11y.atspi.Action').DoAction(0)
            # A real protocol download; receiving alone must not write a file.
            fixture = root / 'download.sh'
            fixture.write_text("printf '\\033]1337;File=name=ZDEtdGVzdC50eHQ=;size=5:aGVsbG8=\\007'\n")
            if args.it2dl:
                source = root / 'd1-test.txt'
                source.write_bytes(b'hello')
                fixture.write_text(shlex.join(['bash', str(args.it2dl.resolve()), str(source)]) + '\n')
            def offer(): ctl('send-text', '--pane', panes()[0]['id'], shlex.join(['bash', str(fixture)]) + '\n')
            offer()
            press('cancel')
            wait_for(lambda: button('cancel') is None)
            assert not (root / 'accepted.txt').exists()
            time.sleep(.5)
            offer()
            wait_for(lambda: button('save'))
            destination = root / 'accepted.txt'
            fields = [(obj, name) for obj, name, role, interfaces in nodes() if 'org.a11y.atspi.EditableText' in interfaces]
            print('Save dialog fields:', [name for _, name in fields])
            field = next(obj for obj, name in fields if 'file name' in name.lower())
            assert dbus.Interface(field, 'org.a11y.atspi.EditableText').SetTextContents(str(destination))
            press('save')
            wait_for(destination.exists)
            assert destination.read_bytes() == b'hello'
            print('PASS: OSC 1337 cancellation and explicit save')
        finally:
            import shutil
            shutil.copyfile(root / 'kwin.log', '/tmp/afterglow-d1-kwin.log') if (root / 'kwin.log').exists() else None
            registry_process.terminate()
            registry_process.wait(timeout=5)
            app.terminate()
            try: app.wait(timeout=5)
            except subprocess.TimeoutExpired: app.kill(); app.wait()
