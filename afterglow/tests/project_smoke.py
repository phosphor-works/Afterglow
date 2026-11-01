#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Project and pane transfer smoke test in a nested KWin on a private D-Bus session.

Requires the nested_desktop dependencies, Bash and dbus-run-session.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from nested_desktop import NestedDesktop, Session, check

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
options = parser.parse_args()
binary = str(options.binary.resolve())
if not options.private_bus:
    sys.exit(subprocess.call(["dbus-run-session", "--", sys.executable, __file__, binary, "--private-bus"]))
with tempfile.TemporaryDirectory(prefix='afterglow-t3-') as tmp:
    root = Path(tmp); (root / '.bashrc').write_text("PS1='T3> '\n")
    conf = root / 'afterglow'; (conf / 'scripts').mkdir(parents=True)
    (conf / 'scripts/probe.luau').write_text('return {on_pane_moved=function(ev) return afterglow.log("MOVED|"..ev.pane.id.."|"..ev.pane.tab_id.."|"..ev.pane.window) end}')
    (conf / 'config.json').write_text(json.dumps({
        'Terminal': {'shell': '/bin/bash', 'env': {'HOME': tmp}, 'confirmClose': False},
        'Control': {'allowReadText': True}, 'QuickTerminal': {'shortcut': ''}, 'Audio': {'enabled': False},
        'Animation': {'cursor': False, 'scroll': False, 'splits': False}}))
    with NestedDesktop(root) as desktop:
        env = dict(desktop.env, QT_FORCE_STDERR_LOGGING='1', QT_LOGGING_TO_CONSOLE='1',
                   QT_LOGGING_RULES='afterglow.scripts.info=true')
        s = Session(binary, env, root / 'application.log')
        ctl, panes, screen, wait, identity = s.ctl, s.panes, s.screen, s.wait, s.identity
        def window_of(pane): return next(p['window'] for p in panes() if p['id'] == pane)
        def moved(pane):
            """The window of the last on_pane_moved event that the probe script logged for a pane."""
            events = re.findall(rf'MOVED\|{pane}\|(\d+)\|(\d+)', s.log_text(100000))
            return int(events[-1][1]) if events else None
        s.start('Afterglow project probe')
        try:
            first = wait(panes)[0]['id']; wait(lambda: 'T3>' in screen(first))
            second = int(ctl('split', '--pane', first, '--right')); wait(lambda: 'T3>' in screen(second))
            firstpid, secondpid = identity(first), identity(second)
            ctl('action', 'swap_left', '--pane', second)
            ctl('move-pane', '0', 'new-tab', '--pane', second)
            wait(lambda: len({p['tab'] for p in panes()}) == 2)
            wait(lambda: moved(second) == window_of(second))
            ctl('move-pane', first, 'right', '--pane', second)
            wait(lambda: len({p['tab'] for p in panes()}) == 1)
            spare = int(ctl('new-tab', '--pane', first))
            ctl('detach-tab', '--pane', first)
            wait(lambda: len({p['window'] for p in panes()}) == 2)
            # The detached pane reports its new window to scripts.
            wait(lambda: moved(first) == window_of(first) != window_of(second))
            ctl('move-pane', spare, 'right', '--pane', second)
            ctl('move-pane', second, 'left', '--pane', first)
            wait(lambda: len({p['window'] for p in panes()}) == 1)
            ctl('close', '--pane', spare)
            wait(lambda: len(panes()) == 2)
            check(identity(first) == firstpid and identity(second) == secondpid)
            ctl('action', 'resize_left', '--pane', second)
            ctl('set-title', '--pane', second, 'build shell')
            third = int(ctl('new-tab', '--pane', first, '--cwd', '/tmp')); wait(lambda: 'T3>' in screen(third))
            original = {first, second, third}
            oldpid = identity(first)
            saved = json.loads(ctl('save-project', 'dev', '--pane', first))
            path = conf / 'projects/dev.json'
            layout = json.loads(path.read_text())
            check(len(layout['tabs']) == 2 and layout['tabs'][0]['tree']['ratio'] != .5, layout)
            same = json.loads(ctl('open-project', 'dev')); check(same['reused'] and len(panes()) == 3)
            error = s.failing_ctl('save-project', 'dev', '--pane', first)
            check('already exists' in error, error)
            fresh = json.loads(ctl('open-project', 'dev', '--new'))
            check(not fresh['reused'] and len(fresh['pane_ids']) == 3 and len(fresh['tab_ids']) == 2, fresh)
            restored = set(fresh['pane_ids'].values()); check(not (original & restored))
            newfirst = fresh['pane_ids'][str(first)]
            check(identity(newfirst) != oldpid)
            ctl('save-project', 'copy', '--pane', newfirst)
            copy = json.loads((conf / 'projects/copy.json').read_text())
            check(copy['tabs'][0]['tree']['ratio'] == layout['tabs'][0]['tree']['ratio'])
            check(copy['tabs'][0]['tree']['second']['pane']['title'] == 'build shell')
            check(copy['tabs'][1]['tree']['pane']['cwd'] == '/tmp')
            def active_position(doc): return next(i for i, t in enumerate(doc['tabs']) if t['id'] == doc['active_tab'])
            check(active_position(copy) == active_position(layout))
            # Invalid files must create no objects or launch commands.
            before = len(panes())
            (conf / 'projects/bad.json').write_text('{bad')
            s.failing_ctl('open-project', 'bad')
            check(len(panes()) == before)
            recovery = {'version': 1, 'active_tab': 'main', 'tabs': [{'id': 'main', 'title': 'Recovery', 'active_pane': 'shell', 'tree': {'direction': 'right', 'ratio': .6, 'first': {'pane': {'id': 'shell', 'cwd': '/this-directory-does-not-exist-afterglow', 'title': 'Fallback', 'command': ['/missing-command-afterglow']}}, 'second': {'pane': {'id': 'command', 'cwd': '/tmp', 'title': 'Status', 'command': ['/bin/sh', '-c', 'printf PROJECT_COMMAND_OK; exit 7']}}}}]}
            (conf / 'projects/recovery.json').write_text(json.dumps(recovery))
            result = json.loads(ctl('open-project', 'recovery'))
            check(len(result['warnings']) == 2, result)
            wait(lambda: 'T3>' in screen(result['pane_ids']['shell']))
            wait(lambda: 'PROJECT_COMMAND_OK' in screen(result['pane_ids']['command']))
            wait(lambda: 'exited with status 7' in screen(result['pane_ids']['command']))
            check(len(panes()) == before + 2)
            # A saved project remains usable across complete application restart.
            s.stop()
            s.start('Afterglow project restored probe')
            wait(panes)
            restarted = json.loads(ctl('open-project', 'dev'))
            check(not restarted['reused'] and len(restarted['pane_ids']) == 3)
            wait(lambda: 'T3>' in screen(restarted['pane_ids'][str(first)]))
            check(identity(restarted['pane_ids'][str(first)]) != oldpid)
            print('PASS: live transfer/swap/detach preserves PTYs and reports moves to scripts; save, duplicate protection, running-project reuse, fresh identities/processes, nested ratios/titles/directories/focus, invalid-file isolation, missing directory/command recovery, held failure output, restart restoration')
        except Exception:
            print(s.log_text(5000), file=sys.stderr)
            raise
        finally:
            s.close()
