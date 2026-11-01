#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Project navigator package smoke test in a nested KWin on a private D-Bus session.

Requires the nested_desktop dependencies, Bash and dbus-run-session.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from nested_desktop import NestedDesktop, Session

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
options = parser.parse_args()
binary = str(options.binary.resolve())
if not options.private_bus:
    sys.exit(subprocess.call(["dbus-run-session", "--", sys.executable, __file__, binary, "--private-bus"]))
with tempfile.TemporaryDirectory(prefix='afterglow-l3-') as tmp:
    root = Path(tmp); (root / '.bashrc').write_text("PS1='T3> '\n")
    conf = root / 'afterglow'; (conf / 'scripts').mkdir(parents=True)
    package = conf / 'scripts/project-navigator'
    source = Path(__file__).resolve().parents[2] / 'data/examples/packages/project-navigator'
    shutil.copytree(source, package)
    project = root / 'repository'; project.mkdir()
    (package / 'config.json').write_text(json.dumps({'roots': [str(project)], 'fd': False, 'zoxide': False}))
    state = conf / 'state/scripts'; state.mkdir(parents=True)
    # Exercise the shipped schema-1 -> schema-2 migration before actions run.
    (state / 'project-navigator.json').write_text(json.dumps({'format': 1, 'version': 1, 'revision': 1, 'value': {'favorites': [str(project)], 'recent': [str(project)]}}))
    (conf / 'config.json').write_text(json.dumps({
        'Terminal': {'shell': '/bin/bash', 'env': {'HOME': tmp}, 'confirmClose': False},
        'Control': {'allowReadText': True}, 'QuickTerminal': {'shortcut': ''}, 'Audio': {'enabled': False},
        'Animation': {'cursor': False, 'scroll': False, 'splits': False}}))
    with NestedDesktop(root) as desktop:
        env = dict(desktop.env, QT_FORCE_STDERR_LOGGING='1', QT_LOGGING_TO_CONSOLE='1',
                   QT_LOGGING_RULES='afterglow.scripts.info=true')
        s = Session(binary, env, root / 'application.log')
        ctl, panes, screen, wait, identity = s.ctl, s.panes, s.screen, s.wait, s.identity
        def navigator(): return s.script('project-navigator') or {}
        def saved(): return json.loads((state / 'project-navigator.json').read_text())
        s.start('Afterglow navigator probe')
        try:
            first = wait(panes)[0]['id']; wait(lambda: 'T3>' in screen(first))
            wait(lambda: navigator().get('status') == 'active')
            assert saved()['version'] == 2 and saved()['revision'] == 2, saved()
            # Actual Luau action -> native launch -> result -> durable history.
            ctl('action', 'script:project-navigator.previous', '--pane', first)
            wait(lambda: len(panes()) == 2)
            wait(lambda: saved()['revision'] == 3)
            launched = next(p for p in panes() if p['id'] != first)
            newpane = launched['id']; wait(lambda: 'T3>' in screen(newpane))
            pid = identity(newpane)
            reused = json.loads(ctl('open-directory', project))
            assert reused['reused'] and reused['pane_id'] == newpane and len(panes()) == 2
            assert identity(newpane) == pid
            ctl('action', 'script:project-navigator.favorite', '--pane', newpane)
            wait(lambda: saved()['revision'] == 4)
            assert str(project) not in saved()['value']['favorites']
            second = int(ctl('split', '--pane', newpane, '--right'))
            wait(lambda: 'T3>' in screen(second))
            ctl('save-project', 'two-pane', '--pane', newpane)
            other = root / 'other'; other.mkdir()
            template = json.loads(ctl('open-directory', other, '--layout', 'two-pane'))
            assert len(template['pane_ids']) == 2, template
            for pane in template['pane_ids'].values():
                wait(lambda: next(p['cwd'] for p in panes() if p['id'] == pane) == str(other))
            before = len(panes())
            s.failing_ctl('open-directory', root / 'missing')
            assert len(panes()) == before
            # Atomic module edits keep the last good package and then recover.
            module = package / 'history.luau'; original = module.read_text()
            module.write_text('invalid syntax !')
            wait(lambda: navigator().get('status') == 'last-good')
            assert navigator()['error']
            module.write_text(original)
            wait(lambda: navigator().get('status') == 'active')
            s.stop()
            s.start('Afterglow navigator restored probe')
            wait(panes)
            wait(lambda: navigator().get('status') == 'active')
            assert saved()['revision'] == 4 and saved()['version'] == 2
            assert saved()['value']['recent'][0]['value'] == str(project)
            print('PASS: installed package, schema migration, Luau previous-project action, launch/reuse preserves PID, persistent favorites, template directories, invalid directory isolation, module hot reload/diagnostics, restart history')
        except Exception:
            print(s.log_text(5000), file=sys.stderr)
            raise
        finally:
            s.close()
