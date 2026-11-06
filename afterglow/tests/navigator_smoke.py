#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Project navigator package smoke test in a nested KWin on a private D-Bus session.

Requires the nested_desktop dependencies, Bash, dbus-run-session, and the aglow
of the build next to afterglow.
"""
import argparse
import json
from pathlib import Path
import shutil
import signal
import sys
import tempfile
from nested_desktop import NestedDesktop, Session, check, exit_on_sigterm, run_on_private_bus

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--timeout", type=float, default=300, help="Seconds for the whole run (default: 300)")
parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
options = parser.parse_args()
binary = str(options.binary.resolve())
if not options.private_bus:
    sys.exit(run_on_private_bus([sys.executable, __file__, binary, "--private-bus"], options.timeout))
exit_on_sigterm()
with tempfile.TemporaryDirectory(prefix='afterglow-l3-') as tmp:
    root = Path(tmp); (root / '.bashrc').write_text("PS1='T3> '\n")
    conf = root / 'afterglow'; (conf / 'scripts').mkdir(parents=True)
    package = conf / 'scripts/project-navigator'
    source = Path(__file__).resolve().parents[2] / 'data/examples/packages/project-navigator'
    shutil.copytree(source, package)
    project = root / 'repository'; project.mkdir()
    (package / 'config.json').write_text(json.dumps({'roots': [str(project)], 'fd': False, 'zoxide': False}))
    state = conf / 'state/scripts'; state.mkdir(parents=True)
    # Exercise the shipped schema-1 -> schema-2 migration before commands run.
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
            check(saved()['version'] == 2 and saved()['revision'] == 2, saved())
            listed = s.run('project-navigator', 'list').stdout
            check(f'recent\t{project}' in listed and f'favorite\t{project}' in listed, listed)
            # A Luau command from the shell -> native launch -> result -> durable
            # history. The run ends once the history is saved.
            s.run('--pane', first, 'project-navigator', 'previous')
            check(saved()['revision'] == 3, saved())
            wait(lambda: len(panes()) == 2)
            launched = next(p for p in panes() if p['id'] != first)
            newpane = launched['id']; wait(lambda: 'T3>' in screen(newpane))
            pid = identity(newpane)
            reused = json.loads(ctl('open-directory', project))
            check(reused['reused'] and reused['pane_id'] == newpane and len(panes()) == 2)
            check(identity(newpane) == pid)
            result = s.run('--pane', newpane, 'project-navigator', 'favorite')
            check(result.stdout == 'Favorite removed\n', result.stdout)
            check(saved()['revision'] == 4 and str(project) not in saved()['value']['favorites'], saved())
            # A usage error and an unknown command exit with status 2.
            for command in ('open', 'nope'):
                result = s.run('project-navigator', command, check_status=False)
                check(result.returncode == 2, f'run project-navigator {command} exited {result.returncode}: {result.stderr}')
            second = int(ctl('split', '--pane', newpane, '--right'))
            wait(lambda: 'T3>' in screen(second))
            ctl('save-project', 'two-pane', '--pane', newpane)
            other = root / 'other'; other.mkdir()
            template = json.loads(ctl('open-directory', other, '--layout', 'two-pane'))
            check(len(template['pane_ids']) == 2, template)
            for pane in template['pane_ids'].values():
                # A pane that is not listed yet has no directory: wait, do not stop.
                wait(lambda: next((p['cwd'] for p in panes() if p['id'] == pane), None) == str(other))
            before = len(panes())
            s.failing_ctl('open-directory', root / 'missing')
            check(len(panes()) == before)
            # A second launch hands its window to this process and exits.
            check(s.launch('--title', 'Afterglow handover probe') == 0, 'second launch did not hand over')
            wait(lambda: len(panes()) == before + 1)
            # A program that cannot start fails the launch with the main
            # process's message.
            check(s.launch('-e', root / 'no-such-program') == 1, 'a failed handover exited 0')
            check(len(panes()) == before + 1)
            # Atomic module edits keep the last good package and then recover.
            module = package / 'history.luau'; original = module.read_text()
            module.write_text('invalid syntax !')
            wait(lambda: navigator().get('status') == 'last-good')
            check(navigator()['error'])
            module.write_text(original)
            wait(lambda: navigator().get('status') == 'active')
            # SIGTERM closes Afterglow cleanly, and its control socket with it.
            check(s.control_socket().exists(), 'no control socket while running')
            s.stop()
            check(s.exit_code == -signal.SIGTERM, f'exit code {s.exit_code} after SIGTERM')
            check(not s.control_socket().exists(), 'control socket left after SIGTERM')
            s.start('Afterglow navigator restored probe')
            wait(panes)
            wait(lambda: navigator().get('status') == 'active')
            check(saved()['revision'] == 4 and saved()['version'] == 2)
            check(saved()['value']['recent'][0]['value'] == str(project))
            print('PASS: installed package, schema migration, aglow run list/previous/favorite with exit statuses, launch/reuse preserves PID, persistent favorites, template directories, invalid directory isolation, module hot reload/diagnostics, restart history, launch handover, failed handover, exit by SIGTERM')
        except Exception:
            print(s.log_text(5000), file=sys.stderr)
            raise
        finally:
            s.close()
