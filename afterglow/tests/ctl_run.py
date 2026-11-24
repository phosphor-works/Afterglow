#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drives `afterglow ctl run`, `wait` and `--wait-text` as a shell agent does: one command for each call.

The run takes its private directories, its daemon and its bash with the prompt `mcp$ ` from mcp_session.py (Home):
no command reaches the user's mux daemon, Afterglow, bus or shell files. Each session case makes a session with
`afterglow mux new --detach` and drives its first pane with `afterglow ctl --session NAME`. Reading pane text
(Control.allowReadText) is on in the run's config.

Without a CASE, every case runs but run_in_a_window, which opens an Afterglow window in a headless KWin of its own, on
a bus of its own, and runs a command in its pane through the socket of that window. It draws on the GPU, and skips
with a reason without kwin_wayland or dbus-run-session, or without a Vulkan device. The exit status is 77 when each
case that ran skipped.

Usage: ctl_run.py AFTERGLOW [CASE...]
"""

import json
import os
import shutil
import subprocess
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcp_session import Failure, Home, Skip, check, sweep_stale_runs, wait_for  # noqa: E402
from nested_desktop import KWINRC, exit_on_sigterm  # noqa: E402

TIMED_OUT = 124
FAILED = 125
USAGE = 2

CASES = []
WINDOW_CASES = set()


def case(function):
    CASES.append(function)
    return function


def window_case(function):
    WINDOW_CASES.add(function.__name__)
    return case(function)


class Ctl:
    """afterglow ctl against session NAME of the run's daemon, or against a window (env)."""

    def __init__(self, home, session=None, env=None):
        self.home = home
        self.session = session
        self.env = env or home.env()

    def __call__(self, *args, timeout=60):
        prefix = ['--session', self.session] if self.session else []
        started = time.monotonic()
        result = subprocess.run([self.home.binary, 'ctl', *prefix, *args], env=self.env, capture_output=True,
                                text=True, timeout=timeout)
        result.took = time.monotonic() - started
        return result

    def panes(self):
        result = self('ls', '--json')
        check(result.returncode == 0, f'ls failed: {result.stderr}')
        return json.loads(result.stdout)

    def at_prompt(self, pane, timeout=15.0):
        def ready():
            facts = [p for p in self.panes() if p['id'] == pane]
            return facts and facts[0]['shell'] == 'prompt' and not facts[0]['input']
        wait_for(ready, timeout, f'pane {pane} shows no prompt: {self.panes()}')


def session(home, name):
    """A new session of the run's daemon, and its first pane at its prompt."""
    home.settings(control={'allowReadText': True})
    made = home.mux('new', '--detach', '--name', name)
    check(made.returncode == 0, f'afterglow mux new failed: {made.stderr}')
    ctl = Ctl(home, name)
    pane = ctl.panes()[0]['id']
    ctl.at_prompt(pane)
    return ctl, str(pane)


@case
def run_gives_the_exit_code_and_the_output(home):
    ctl, pane = session(home, 'codes')
    result = ctl('run', '--pane', pane, 'echo hi; false')
    check(result.returncode == 1 and 'hi\n' in result.stdout and result.stderr == '', result)
    # Bash runs each line as a command of its own: a code for each.
    result = ctl('run', '--pane', pane, '--json', 'true\n(exit 4)')
    check(result.returncode == 4, result)
    answer = json.loads(result.stdout)
    check(answer['status'] == 'finished' and answer['exit_codes'] == [0, 4] and answer['notes'] == [], answer)
    # Words after -- are the command, also those that look like options.
    result = ctl('run', '--pane', pane, '--', 'echo', '--json')
    check(result.returncode == 0 and '--json\n' in result.stdout, result)
    # A comment runs nothing: no code, and the status 0.
    result = ctl('run', '--pane', pane, '# nothing')
    check(result.returncode == 0, result)


@case
def a_timeout_then_wait(home):
    ctl, pane = session(home, 'later')
    result = ctl('run', '--pane', pane, '--timeout', '500', 'sleep 1.5; echo late; (exit 7)')
    check(result.returncode == TIMED_OUT and 'afterglow ctl wait --until finished' in result.stderr, result)
    # The command runs on: a run is refused, with the words of afterglow ctl.
    result = ctl('run', '--pane', pane, 'true')
    check(result.returncode == FAILED and 'afterglow ctl send-key ctrl+c' in result.stderr, result)
    result = ctl('wait', '--pane', pane, '--until', 'finished', '--timeout', '10000')
    check(result.returncode == 7 and 'late' in result.stdout, result)
    # quiet: no new text for that long.
    result = ctl('wait', '--pane', pane, '--until', 'quiet', '--quiet-ms', '400', '--timeout', '5000')
    check(result.returncode == 0, result)
    result = ctl('wait', '--pane', pane, '--until', 'text', '--pattern', 'never-there', '--timeout', '500')
    check(result.returncode == TIMED_OUT, result)


@case
def an_incomplete_command_is_refused(home):
    ctl, pane = session(home, 'open')
    result = ctl('run', '--pane', pane, 'echo "open')
    check(result.returncode == FAILED and 'incomplete' in result.stderr
          and 'afterglow ctl send-key ctrl+c' in result.stderr, result)
    check(ctl('send-key', '--pane', pane, 'ctrl+c').returncode == 0, 'no ctrl+c')
    ctl.at_prompt(int(pane))
    check(ctl('run', '--pane', pane, 'true').returncode == 0, 'no run after ctrl+c')


@case
def send_then_wait_for_the_answer(home):
    """A prompt that ends with a space, as Python's does, and an answer a second after the input: the wait reads
    from where the input went, so the prompt that it answered does not match, and prints the whole answer."""
    ctl, pane = session(home, 'repl')
    made = ctl('new-tab', '--pane', pane, '--', home.bash, '--norc', '-c',
               'while :; do printf "> "; IFS= read -r l || exit; sleep 1; echo "got $l"; done')
    check(made.returncode == 0, made)
    repl = made.stdout.strip()
    result = ctl('wait', '--pane', repl, '--until', 'text', '--pattern', '^> $', '--timeout', '10000')
    check(result.returncode == 0, result)
    result = ctl('send-text', '--pane', repl, 'x\\n', '--wait-text', '^> $', '--timeout', '10000')
    check(result.returncode == 0 and 'got x' in result.stdout and result.took >= 0.8, (result, result.took))
    result = ctl('send-key', '--pane', repl, '--json', 'y', 'enter', '--wait-text', '^got y$', '--timeout', '10000')
    answer = json.loads(result.stdout)
    check(result.returncode == 0 and answer['match'] == 'got y', result)


@case
def usage_errors(home):
    ctl = Ctl(home, 'none')
    for args, words in ((['run'], 'run needs a command'),
                        (['wait'], 'wait needs --until'),
                        (['wait', '--until', 'soon'], 'wait needs --until'),
                        (['wait', '--until', 'text'], 'needs --pattern'),
                        (['wait', '--until', 'text', '--pattern', '(open'], 'not a valid regular expression'),
                        (['wait', '--until', 'finished', '--pattern', 'x'], '--pattern goes with --until text'),
                        (['wait', '--until', 'quiet'], 'needs --quiet-ms'),
                        (['run', '--timeout', '0', 'true'], '--timeout takes milliseconds'),
                        (['run', '--tail', 'x', 'true'], '--tail takes'),
                        (['send-text', 'x', '--timeout', '100'], 'go with --wait-text'),
                        (['send-key', 'enter', '--wait-text', '[x'], 'not a valid regular expression')):
        result = ctl(*args)
        check(result.returncode == USAGE and words in result.stderr, (args, result))


@window_case
def run_in_a_window(home):
    """A pane of an Afterglow window, through the socket of the window: the window checks the prompt and writes the
    command in one step, as the daemon does."""
    for program in ('kwin_wayland', 'dbus-run-session'):
        if not shutil.which(program):
            raise Skip(f'{program} is not in PATH')
    home.settings(control={'allowReadText': True})
    (home.config / 'kwinrc').write_text(KWINRC)
    display = f'aglow-ctl-{os.getpid()}'
    env = home.env()
    kwin = subprocess.Popen(['dbus-run-session', '--', 'kwin_wayland', '--virtual', '--socket', display,
                             '--width', '1100', '--height', '750', '--no-lockscreen', '--no-global-shortcuts',
                             '--no-kactivities'], env=env, stdout=home.log_file('kwin'), stderr=subprocess.STDOUT,
                            start_new_session=True)
    window = None
    try:
        try:
            wait_for(lambda: (home.runtime / display).exists() or kwin.poll() is not None, 15)
        except Failure:
            raise Skip('the headless KWin made no display in 15 s')
        if kwin.poll() is not None:
            raise Skip('the headless KWin did not start (no GPU?)')
        window_env = dict(env, WAYLAND_DISPLAY=display, QT_QPA_PLATFORM='wayland', QT_QPA_PLATFORMTHEME='generic',
                          QT_QUICK_CONTROLS_STYLE='Basic')
        window = subprocess.Popen(['dbus-run-session', '--', home.binary], env=window_env,
                                  stdout=home.log_file('window'), stderr=subprocess.STDOUT, start_new_session=True)
        ctl = Ctl(home, env=window_env)

        def listed():
            if window.poll() is not None:
                return True
            result = ctl('ls', '--json')
            return result.returncode == 0 and json.loads(result.stdout)
        wait_for(listed, 30, 'the window lists no pane')
        if window.poll() is not None:
            tail = (home.root / 'window.log').read_text(errors='replace')[-600:]
            if 'failed to create the Vulkan' in tail:
                raise Skip(f'the window has no Vulkan device: {tail}')
            raise Failure(f'the window exited at its start: {tail}')
        pane = ctl.panes()[0]['id']
        ctl.at_prompt(pane)
        result = ctl('run', '--pane', str(pane), 'echo hi; false')
        check(result.returncode == 1 and 'hi\n' in result.stdout, result)
        result = ctl('run', '--pane', str(pane), '--timeout', '300', 'sleep 2')
        check(result.returncode == TIMED_OUT, result)
        # The window refuses the paste of a run while a command runs.
        result = ctl('run', '--pane', str(pane), 'true')
        check(result.returncode == FAILED and 'runs a command' in result.stderr, result)
        result = ctl('wait', '--pane', str(pane), '--until', 'finished', '--timeout', '10000')
        check(result.returncode == 0, result)
    finally:
        for process in (window, kwin):
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(10)
                except subprocess.TimeoutExpired:
                    process.kill()


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    binary = str(Path(sys.argv[1]).resolve())
    chosen = sys.argv[2:]
    names = {function.__name__ for function in CASES}
    unknown = [name for name in chosen if name not in names]
    if unknown:
        print(f'no case is named {", ".join(unknown)}. The cases: {", ".join(sorted(names))}', file=sys.stderr)
        return 2
    exit_on_sigterm()
    sweep_stale_runs()
    home = Home(binary)
    failures = ran = skipped = 0
    try:
        for function in CASES:
            name = function.__name__
            if (chosen and name not in chosen) or (not chosen and name in WINDOW_CASES):
                continue
            ran += 1
            started = time.monotonic()
            try:
                function(home)
                print(f'PASS {name} ({time.monotonic() - started:.1f} s)', flush=True)
            except Skip as skip:
                skipped += 1
                print(f'SKIP {name}: {skip}', flush=True)
            except Exception:
                failures += 1
                print(f'FAIL {name}:\n{traceback.format_exc()}', flush=True)
            finally:
                home.settings()
                # Each case starts with no session of the ones before.
                home.end_sessions()
    finally:
        home.close()
    if failures:
        print(f'{failures} failed')
        return 1
    if ran and skipped == ran:
        print('all skipped')
        return 77
    print('all passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
