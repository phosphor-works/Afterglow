#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native AT-SPI text, selection and pane actions, and the question of an OSC 1337 download: Refuse, Save As with a
name typed in the Save dialog, and Save As then Cancel.
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
import dbus
from nested_desktop import NestedDesktop, check, exit_on_sigterm, run_on_private_bus, wait_for

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--private-bus', action='store_true')
parser.add_argument('--it2dl', type=Path, help='Optional upstream it2dl script to exercise instead of the inline fixture')
args = parser.parse_args()
binary = str(args.binary.resolve())
if not args.private_bus:
    # At the time limit, the whole run stops: KWin, Afterglow and the registry too (run_on_private_bus).
    sys.exit(run_on_private_bus([sys.executable, '-u', __file__, binary, '--private-bus']
                                + (['--it2dl', str(args.it2dl.resolve())] if args.it2dl else []), 600))
# A SIGTERM of the time limit runs the finally blocks below, which stop what this run started.
exit_on_sigterm()
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
    config = root / 'phosphor-works/afterglow'
    config.mkdir(parents=True)
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
        # HOME is the temporary root, and the download directory, where the
        # Save dialog opens, is root/downloads (NestedDesktop's
        # user-dirs.dirs): no download can reach the user's own files.
        env = dict(desktop.env, HOME=tmp, QT_LINUX_ACCESSIBILITY_ALWAYS_ON='1', QT_ACCESSIBILITY='1', QT_QPA_PLATFORMTHEME='generic', QT_LOGGING_TO_CONSOLE='1', QT_FORCE_STDERR_LOGGING='1')
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
                    try:
                        obj = bus.get_object(str(ref[0]), str(ref[1]))
                        iface = dbus.Interface(obj, A)
                        if str(iface.GetRoleName()) == 'terminal': found.append(obj)
                        for child in iface.GetChildren(): visit(child, depth + 1)
                    except dbus.exceptions.DBusException:
                        return  # The window can still be making its children.
                for child in dbus.Interface(registry, A).GetChildren(): visit(child)
                return found
            terminal = wait_for(lambda: next(iter(terminals()), None), timeout=20)
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
            # The pane's palette actions, as a screen reader lists and runs them.
            action = dbus.Interface(terminal, 'org.a11y.atspi.Action')
            names = [str(action.GetName(i)) for i in range(int(dbus.Interface(terminal, P).Get('org.a11y.atspi.Action', 'NActions')))]
            # Qt's bridge names its standard menu and focus actions "Show Menu"
            # and "Set Focus": the menu first, the focus last.
            check(names[:1] == ['Show Menu'] and names[-1:] == ['Set Focus'] and 'Copy' in names and 'Split Right' in names
                  and 'Next Tab' not in names, names)
            # Each entry is the name, the description and the first key binding.
            # The name is the palette title, which a screen reader says. It
            # has no description that would only say the title again.
            entries = [tuple(map(str, entry)) for entry in action.GetActions()]
            check(entries[names.index('Split Right')][1] == '', entries)
            # The default chord: the fixture's configuration binds no keys.
            check(entries[names.index('Copy')][2] == 'Ctrl+Shift+C', entries)
            before = [p['id'] for p in panes()]
            check(action.DoAction(names.index('Split Right')))
            split = wait_for(lambda: [p['id'] for p in panes() if p['id'] not in before])
            ctl('close', '--pane', split[0])
            wait_for(lambda: len(panes()) == len(before))
            print('PASS: AT-SPI pane actions')
            def nodes(under=None):
                result = []
                def visit(obj, depth=0):
                    if depth > 12: return
                    try:
                        accessible = dbus.Interface(obj, A)
                        name = str(dbus.Interface(obj, P).Get(A, 'Name'))
                        result.append((obj, name, str(accessible.GetRoleName()), list(accessible.GetInterfaces())))
                        for child in accessible.GetChildren(): visit(bus.get_object(str(child[0]), str(child[1])), depth+1)
                    except dbus.exceptions.DBusException:
                        return  # Dialog children can disappear between queries.
                if under is not None: visit(under)
                else:
                    for ref in dbus.Interface(registry, A).GetChildren(): visit(bus.get_object(str(ref[0]), str(ref[1])))
                return result
            def dialog(prefix):
                return next((obj for obj, name, role, interfaces in nodes() if name.startswith(prefix)), None)
            def save_dialog(): return dialog('Save file received from')
            # The question of a download comes first, before its data
            # (Terminal.downloads is ask): a box named Download.
            def question(): return dialog('Download')
            # A dialog's own buttons: the window's chrome has a Cancel and a
            # Save of its own prompts too.
            def button(name, of=save_dialog):
                found = of()
                for obj, label, role, interfaces in (nodes(found) if found is not None else []):
                    if label.replace('&', '').lower() == name and role == 'push button': return obj
                return None
            def press(name, of=save_dialog):
                obj = wait_for(lambda: button(name, of))
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
            # The download, then a marker that its shell prints after it. The output after the download waits for
            # the answer: once the marker shows, the answer has gone. The marker is arithmetic, so that the command
            # line that the shell echoes does not show it.
            def offer(marker):
                ctl('send-text', '--pane', panes()[0]['id'], shlex.join(['bash', str(fixture)]) + f'; echo {marker}-$((40+2))\n')
            def answered(marker): wait_for(lambda: f'{marker}-42' in str(text.GetText(0, -1)), timeout=10)
            def written(): return [p for p in root.rglob(offered) if p != source]
            offer('REFUSED')
            wait_for(question)
            press('refuse', question)
            wait_for(lambda: question() is None)
            answered('REFUSED')
            # HOME is the temporary root, so every place that a download can go is under it.
            check(not written(), written())
            check(not (root / 'accepted.txt').exists())
            offer('SAVED')
            press('save as…', question)
            wait_for(lambda: button('save'), timeout=10)
            destination = root / 'accepted.txt'
            fields = [(obj, name) for obj, name, role, interfaces in nodes(save_dialog()) if 'org.a11y.atspi.EditableText' in interfaces]
            print('Save dialog fields:', [name for _, name in fields])
            field = next((obj for obj, name in fields if 'file name' in name.lower()), None)
            check(field is not None, [name for _, name in fields])
            check(dbus.Interface(field, 'org.a11y.atspi.EditableText').SetTextContents(str(destination)))
            press('save')
            wait_for(destination.exists)
            check(destination.read_bytes() == b'hello')
            answered('SAVED')
            check(not list(root.rglob('*.part')), list(root.rglob('*.part')))
            # Save As, then Cancel in the Save dialog: no file, and the program goes on.
            offer('CANCELLED')
            press('save as…', question)
            wait_for(lambda: button('cancel'), timeout=10)
            press('cancel')
            wait_for(lambda: save_dialog() is None)
            answered('CANCELLED')
            check(not written(), written())
            check(not list(root.rglob('*.part')), list(root.rglob('*.part')))
            print('PASS: OSC 1337 question, refusal, explicit save, and a cancelled Save dialog')
        finally:
            if (root / 'kwin.log').exists():
                shutil.copyfile(root / 'kwin.log', logs / 'kwin.log')
            for process in (registry_process, app):
                if process is None:
                    continue
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
