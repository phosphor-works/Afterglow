#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drives `afterglow mcp` as an MCP client does: one JSON-RPC message per line on its standard input and output.

Each server runs with a private XDG_RUNTIME_DIR, XDG_STATE_HOME, XDG_CONFIG_HOME and HOME under /tmp, and only the
variables of an allow list, so that no server reaches the user's mux daemon, Afterglow, bus or shell files. The first
server of a case starts a private daemon, which ends with the last session of the case: each case starts with no
session and no daemon. Bash is the shell of the session, with the shell integration of the source tree (an afterglow
in a build tree reads it) and an rc file that sets the prompt that the checks wait for. Bash is required. The zsh and
gdb cases skip with a reason when the program is not in PATH. At the end, on a failure and at SIGTERM, the run ends
every server, session, window and daemon that it started. A run that SIGKILL ended leaves its directory: the next
run ends what still runs there, and removes it.

Without a CASE, every case runs but script_command_through_a_window, which opens a window of the session in a
headless KWin of its own, on a bus of its own, and draws on the GPU. It skips with a reason without kwin_wayland or
dbus-run-session, or without a Vulkan device. A name that is not a case fails. The exit status is 77 when each case
that ran skipped.

Usage: mcp_session.py AFTERGLOW [CASE...]
"""
import json
import os
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path

# One KWin configuration for every runner: the user's KWin plugins must not
# load into the test compositor.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from nested_desktop import KWINRC, exit_on_sigterm  # noqa: E402

VERSION = '2026-07-28'
META = {'io.modelcontextprotocol/protocolVersion': VERSION, 'io.modelcontextprotocol/clientCapabilities': {}}
PROMPT = 'mcp$ '
ALLOWED = {'PATH', 'LANG', 'LANGUAGE', 'TZ', 'USER', 'LOGNAME', 'LD_LIBRARY_PATH'}
ROOT_PREFIX = 'aglow-mcp-'


class Failure(Exception):
    pass


class Timeout(Failure):
    """No message came in time: the server is alive, and can still answer."""


class Skip(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def processes_of(runtime, skip=()):
    """The processes whose environment names the runtime directory @runtime: a daemon, its panes, a KWin."""
    marker = f'XDG_RUNTIME_DIR={runtime}'.encode()
    found = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit() or int(entry.name) in skip:
            continue
        try:
            if marker in (entry / 'environ').read_bytes().split(b'\0'):
                found.append(int(entry.name))
        except OSError:
            continue
    return found


def kill_all(pids):
    for pid in pids:
        try:
            os.kill(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def sweep_stale_runs():
    """Ends what a run that SIGKILL ended left (a ctest timeout): the processes in its directories, and the
    directories. A run that still runs keeps its own."""
    for root in Path('/tmp').glob(ROOT_PREFIX + '*'):
        try:
            runner = int((root / 'runner.pid').read_text())
            os.kill(runner, 0)
            continue
        except ProcessLookupError:
            pass
        except (OSError, ValueError):
            # No pid file: one of another user, or a run that makes its
            # directory now, whose pid file comes at once.
            try:
                if not os.access(root, os.W_OK) or time.time() - root.stat().st_mtime < 60:
                    continue
            except OSError:
                continue
        kill_all(processes_of(root / 'run', skip={os.getpid()}))
        shutil.rmtree(root, ignore_errors=True)


def wait_for(probe, timeout=10.0, message='timed out'):
    deadline = time.monotonic() + timeout
    while True:
        value = probe()
        if value:
            return value
        if time.monotonic() > deadline:
            raise Failure(message)
        time.sleep(0.05)


class Home:
    """The private directories of the run, its configuration, and the daemon that its servers start."""

    def __init__(self, binary):
        self.binary = binary
        self.bash = shutil.which('bash')
        if not self.bash:
            raise Failure('bash is not in PATH: the run needs it')
        # Under /tmp: a socket path has at most 107 bytes.
        self.root = Path(tempfile.mkdtemp(prefix=ROOT_PREFIX, dir='/tmp'))
        (self.root / 'runner.pid').write_text(str(os.getpid()))
        self.runtime = self.root / 'run'
        self.home = self.root / 'home'
        self.config = self.root / 'config'
        # Afterglow's folder in XDG_CONFIG_HOME: the suite's folder holds it.
        self.app = self.config / 'phosphor-works' / 'afterglow'
        for directory in (self.runtime, self.home, self.app, self.root / 'state'):
            directory.mkdir(parents=True, mode=0o700)
        (self.home / '.bashrc').write_text(f"PS1='{PROMPT}'\nHISTFILE=/dev/null\n")
        (self.home / '.zshrc').write_text("PS1='mcp%# '\nHISTFILE=/dev/null\n")
        self.logs = []
        self.servers = []
        self.settings()

    def env(self):
        env = {k: v for k, v in os.environ.items()
               if k in ALLOWED or k.startswith('LC_') or (k.endswith('SAN_OPTIONS') and len(k) > 11)}
        env.update(XDG_RUNTIME_DIR=str(self.runtime), XDG_STATE_HOME=str(self.root / 'state'),
                   XDG_CONFIG_HOME=str(self.config), XDG_DATA_HOME=str(self.root / 'data'),
                   XDG_CACHE_HOME=str(self.root / 'cache'), HOME=str(self.home), SHELL=self.bash)
        env.setdefault('PATH', '/usr/bin:/bin')
        return env

    def settings(self, shell=None, control=None, **mcp):
        """The config file that the next server reads, once, when it starts."""
        config = {'Terminal': {'shell': shell or self.bash, 'confirmClose': False},
                  'Control': dict({'eventInterval': 100}, **(control or {})), 'Mcp': mcp}
        (self.app / 'config.json').write_text(json.dumps(config))

    def log_file(self, name):
        log = open(self.root / f'{name}.log', 'w')
        self.logs.append(log)
        return log

    def server(self, *args, check_start=True):
        server = Server(self, list(args))
        self.servers.append(server)
        if check_start:
            server.wait_started()
        return server

    def daemon(self, fn, args=(), absent=None):
        """One call of the daemon's mux target, as a raw phosphor-ipc client: its answer, or None when no daemon
        answers (it closed the connection, or it sent no line in 5 s). @p absent when none runs: its socket is
        gone, or refuses the connection."""
        path = self.runtime / 'aglow-mux' / 'socket'
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                s.settimeout(5)
                try:
                    s.connect(str(path))
                except (FileNotFoundError, ConnectionRefusedError):
                    return absent
                s.sendall((json.dumps({'type': 'call', 'id': 1, 'target': 'mux', 'fn': fn, 'args': list(args)})
                           + '\n').encode())
                data = b''
                while not data.endswith(b'\n'):
                    chunk = s.recv(65536)
                    if not chunk:
                        return None
                    data += chunk
            return json.loads(data.splitlines()[0])
        except (OSError, ValueError):
            return None

    def sessions(self):
        """The sessions of the daemon by name. A daemon that does not answer fails the check: no session ends
        there, and a check that one ended would pass. One that does not run has none: the daemon exits after
        the end of its last session, as soon as it has no connection."""
        reply = self.daemon('listSessions', absent={'result': []})
        check(reply is not None and 'result' in reply, f'the daemon did not list its sessions: {reply}')
        return {s['name']: s for s in reply['result']}

    def sessions_if_any(self):
        """The sessions, or none when no daemon runs: the end of a case and of the run."""
        reply = self.daemon('listSessions')
        return {s['name']: s for s in reply.get('result', [])} if reply else {}

    def settle(self):
        """Two calls of the daemon, one after the other: the end of a connection that closed before them, which
        the daemon reads in the same turn of its loop as the first call at most, is done by the second."""
        self.sessions()
        self.sessions()

    def mux(self, *args):
        return subprocess.run([self.binary, 'mux', *args], env=self.env(), capture_output=True, text=True, timeout=30)

    def our_processes(self):
        """The processes whose environment names this run's runtime directory: the daemon and the panes."""
        return processes_of(self.runtime, skip={os.getpid()})

    def end_sessions(self):
        for name in self.sessions_if_any():
            self.daemon('killSession', [name])

    def close(self):
        """Ends all that the run started, each step also when one before it failed, and removes the directory."""
        try:
            for server in self.servers:
                try:
                    server.kill()
                except Exception:
                    traceback.print_exc()
            for log in self.logs:
                log.close()
            self.end_sessions()
            # The daemon ends with its last session. One that does not, and
            # every pane of this run, ends here.
            try:
                wait_for(lambda: not self.our_processes(), 5)
            except Failure:
                kill_all(self.our_processes())
        finally:
            shutil.rmtree(self.root, ignore_errors=True)


class Server:
    def __init__(self, home, args):
        self.home = home
        self.args = args
        self.log = open(home.root / f'server-{len(home.servers)}.log', 'w+')
        self.process = subprocess.Popen([home.binary, 'mcp', *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, env=home.env(), cwd=home.home)
        self.buffer = b''
        self.next_id = 0
        self.replies = {}
        self.notifications = []
        self.session = None

    def stderr(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read()

    def write(self, message):
        line = message if isinstance(message, bytes) else json.dumps(message).encode()
        self.process.stdin.write(line + b'\n')
        self.process.stdin.flush()

    def request(self, method, params=None, meta=META, **extra_meta):
        self.next_id += 1
        params = dict(params or {})
        if meta is not None:
            params['_meta'] = dict(meta, **extra_meta)
        self.write({'jsonrpc': '2.0', 'id': self.next_id, 'method': method, 'params': params})
        return self.next_id

    def read_message(self, deadline):
        while b'\n' not in self.buffer:
            left = deadline - time.monotonic()
            if left <= 0:
                raise Timeout(f'no message from the server; its stderr:\n{self.stderr()}')
            ready, _, _ = select.select([self.process.stdout], [], [], left)
            if not ready:
                continue
            chunk = os.read(self.process.stdout.fileno(), 1 << 20)
            if not chunk:
                raise Failure(f'the server closed its output; its stderr:\n{self.stderr()}')
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b'\n', 1)
        return json.loads(line)

    def reply(self, id, timeout=30.0):
        deadline = time.monotonic() + timeout
        while id not in self.replies:
            message = self.read_message(deadline)
            if 'id' in message and 'method' not in message:
                self.replies[message['id']] = message
            else:
                self.notifications.append(message)
        return self.replies.pop(id)

    def start_call(self, name, /, progress=None, **arguments):
        extra = {'progressToken': progress} if progress is not None else {}
        return self.request('tools/call', {'name': name, 'arguments': arguments}, **extra)

    def result(self, id, timeout=30.0):
        reply = self.reply(id, timeout)
        check('result' in reply, f'a protocol error: {reply}')
        return reply['result']

    def call(self, name, timeout=30.0, /, **arguments):
        return self.result(self.start_call(name, **arguments), timeout)

    def tool(self, name, timeout=30.0, /, **arguments):
        """A call that must succeed: its structured content, or its text."""
        result = self.call(name, timeout, **arguments)
        check(not result.get('isError'), f'{name} {arguments} failed: {result["content"][0]["text"]}')
        return result.get('structuredContent', result['content'][0]['text'])

    def error(self, name, timeout=30.0, /, **arguments):
        """A call that must fail as a tool: its text."""
        result = self.call(name, timeout, **arguments)
        check(result.get('isError'), f'{name} {arguments} did not fail: {result}')
        return result['content'][0]['text']

    def wait_started(self):
        listed = self.tool('list_panes')
        self.session = listed['session']
        return listed

    def panes(self):
        return {p['id']: p for p in self.tool('list_panes')['panes']}

    def first_pane(self):
        return min(self.panes())

    def at_prompt(self, pane, timeout=15.0):
        """Waits until the shell of the pane shows its prompt with no typed text."""
        def ready():
            facts = self.panes().get(pane)
            return facts and facts['shell'] == 'prompt' and not facts['input']
        wait_for(ready, timeout, f'pane {pane} shows no prompt: {self.panes().get(pane)}; {self.stderr()}')

    def close(self, timeout=10.0):
        """Ends the server as a client does: the end of its standard input."""
        if self.process.poll() is None:
            self.process.stdin.close()
            self.process.wait(timeout)
        return self.process.returncode

    def kill(self, sig=signal.SIGKILL):
        try:
            if self.process.poll() is None:
                self.process.send_signal(sig)
                self.process.wait(10)
        finally:
            for pipe in (self.process.stdin, self.process.stdout):
                try:
                    pipe.close()
                except OSError:
                    pass
            self.log.close()


CASES = []
# The cases that run only when a run names them: they need a window, and a GPU.
WINDOW_CASES = set()


def case(function):
    CASES.append(function)
    return function


def window_case(function):
    WINDOW_CASES.add(function.__name__)
    return case(function)


@case
def discover_and_tools(home):
    """One server/discover, a bad version and the tool list end to end: the wiring, not the library."""
    s = home.server()
    reply = s.reply(s.request('server/discover'))
    discover = reply['result']
    check(discover['supportedVersions'] == [VERSION], discover)
    # The same instructions for every session, which a client caches for a minute.
    check(s.session not in discover['instructions'] and 'list_panes' in discover['instructions'], discover)
    info = discover['_meta']['io.modelcontextprotocol/serverInfo']
    check(info['name'] == 'afterglow', discover)
    # The release and a hash of the tools and the instructions: a client that keys its cache on the version reads
    # them again after a build that changed them.
    check(re.fullmatch(r'\d+\.\d+\.\d+\+[0-9a-f]{8}', info['version']), info)
    check(discover['cacheScope'] == 'public' and discover['ttlMs'] == 60000, discover)
    bad = s.reply(s.request('tools/list', meta=dict(META, **{'io.modelcontextprotocol/protocolVersion': '2025-11-25'})))
    check(bad['error']['code'] == -32022 and bad['error']['data']['requested'] == '2025-11-25', bad)
    tools = s.reply(s.request('tools/list'))['result']['tools']
    names = [t['name'] for t in tools]
    check(names == ['list_panes', 'open_pane', 'run', 'wait', 'send', 'read', 'screenshot', 'close_pane',
                    'open_project', 'save_project', 'list_script_commands', 'run_script_command'], names)
    expected = {'list_panes': (True, False), 'open_pane': (False, False), 'run': (False, True),
                'wait': (True, False), 'send': (False, True), 'read': (True, False),
                'screenshot': (True, False), 'close_pane': (False, True), 'open_project': (False, False),
                'save_project': (False, True), 'list_script_commands': (True, False),
                'run_script_command': (False, True)}
    for tool in tools:
        hints = tool['annotations']
        check(set(hints) >= {'readOnlyHint', 'destructiveHint', 'idempotentHint', 'openWorldHint'}, tool)
        check((hints['readOnlyHint'], hints['destructiveHint']) == expected[tool['name']], tool)
        check(hints['openWorldHint'] is False, tool)
    # A call without its pane: the library refuses it as a tool error.
    text = s.error('read')
    check('pane' in text, text)


@case
def exit_codes(home):
    s = home.server()
    pane = s.first_pane()
    s.at_prompt(pane)
    for command, code in (('true', 0), ('false', 1), ('(exit 3)', 3)):
        result = s.tool('run', pane=pane, command=command)
        check(result['status'] == 'finished' and result['exit_code'] == code and result['exit_codes'] == [code],
              (command, result))
        check(isinstance(result['duration_ms'], int), result)
    # Two lines: bash runs each as a command of its own, with an output and
    # an exit code each (the D of PS0 ends the line before).
    result = s.tool('run', pane=pane, command='echo o""ne\n(exit 4)')
    check(result['exit_codes'] == [0, 4] and result['exit_code'] == 4, result)
    check('one' in result['output'] and result['output'].rstrip().endswith(PROMPT.rstrip()), result)
    # A compound command over several lines is complete in the paste.
    result = s.tool('run', pane=pane, command='for i in 1 2; do\n  echo "n$i"\ndone')
    check(result['status'] == 'finished' and 'n2' in result['output'], result)
    # An empty line runs nothing, and ends at the new prompt. So does a comment.
    result = s.tool('run', pane=pane, command='')
    check(result['status'] == 'finished' and result['exit_code'] is None and result['exit_codes'] == [], result)
    check(s.tool('run', pane=pane, command='# nothing')['status'] == 'finished', 'a comment did not end')


@case
def incomplete_command(home):
    s = home.server()
    pane = s.first_pane()
    s.at_prompt(pane)
    text = s.error('run', pane=pane, command='echo "')
    check('incomplete' in text, text)
    check(s.panes()[pane]['shell'] == 'continuation', s.panes()[pane])
    text = s.error('run', pane=pane, command='true')
    check('rest of a command' in text, text)
    s.tool('send', pane=pane, keys=['ctrl+c'])
    s.at_prompt(pane)
    check(s.tool('run', pane=pane, command='true')['exit_code'] == 0, 'no run after ctrl+c')


@case
def timeout_then_wait(home):
    s = home.server()
    pane = s.first_pane()
    s.at_prompt(pane)
    result = s.tool('run', pane=pane, command='sleep 2; echo do""ne', timeout_ms=1000)
    check(result['status'] == 'running' and result['exit_code'] is None, result)
    # The command still runs: run refuses the pane.
    check('runs a command' in s.error('run', pane=pane, command='true'), 'a run in a busy pane')
    result = s.tool('wait', pane=pane, until='finished', timeout_ms=10000)
    check(result['status'] == 'finished' and result['exit_code'] == 0 and 'done' in result['output'], result)
    check(isinstance(result['duration_ms'], int) and result['duration_ms'] >= 1500, result)
    # At a prompt with no run: the last command, at once.
    result = s.tool('wait', pane=pane, until='finished', timeout_ms=10000)
    check(result['status'] == 'finished' and result['exit_code'] == 0 and result['duration_ms'] is None, result)


@case
def wait_from_a_new_server(home):
    """A server that kept its session (Mcp.endSessionOnExit false) leaves a command running: the next one waits."""
    home.settings(endSessionOnExit=False)
    first = home.server()
    pane = first.first_pane()
    first.at_prompt(pane)
    result = first.tool('run', pane=pane, command='sleep 2; (exit 5)', timeout_ms=500)
    check(result['status'] == 'running', result)
    check(first.close() == 0, first.stderr())
    check(first.session in home.sessions(), 'the session ended with the server')
    home.settings()
    second = home.server('--session', first.session)
    check(second.session == first.session, second.session)
    result = second.tool('wait', pane=pane, until='finished', timeout_ms=10000)
    check(result['status'] == 'finished' and result['exit_code'] == 5, result)
    second.close()
    # Not bound by the first, and bound by the second: it ended with it.
    wait_for(lambda: first.session not in home.sessions(), 5, 'the session outlived its bound server')


@case
def interrupt_and_signal(home):
    s = home.server()
    pane = s.first_pane()
    s.at_prompt(pane)
    for how, code in (({'keys': ['ctrl+c']}, 130), ({'signal': 'TERM'}, 143)):
        check(s.tool('run', pane=pane, command='sleep 100', timeout_ms=1000)['status'] == 'running', how)
        s.tool('send', pane=pane, **how)
        result = s.tool('wait', pane=pane, until='finished', timeout_ms=10000)
        check(result['exit_code'] == code, (how, result))


@case
def output_limit(home):
    command = "printf '%02000d\\n' 0"
    home.settings(outputLimit=8192)
    s = home.server()
    pane = s.first_pane()
    s.at_prompt(pane)
    whole = s.tool('run', pane=pane, command=command, tail_lines=1000000)
    check(not whole['truncated'], whole)
    size = len(whole['output'].encode())
    s.close()
    for limit, truncated in ((size, False), (size - 1, True)):
        home.settings(outputLimit=limit)
        s = home.server()
        pane = s.first_pane()
        s.at_prompt(pane)
        result = s.tool('run', pane=pane, command=command, tail_lines=1000000)
        check(result['truncated'] == truncated, (limit, size, result))
        check(len(result['output'].encode()) <= limit, (limit, result))
        # Cut from the start: the end, with the next prompt, stays.
        check(result['output'] == whole['output'][len(whole['output']) - len(result['output']):], result)
        s.close()
    home.settings()
    s = home.server()
    pane = s.first_pane()
    s.at_prompt(pane)
    result = s.tool('run', pane=pane, command='i=0; while [ $i -lt 50 ]; do echo row$i; i=$((i+1)); done',
                    tail_lines=5)
    lines = result['output'].split('\n')
    check(result['truncated'] and len(lines) == 5 and lines[0] == 'row46', result)


@case
def wait_for_text(home):
    s = home.server()
    pane = s.first_pane()
    s.at_prompt(pane)
    # A line that 200 more lines push off the screen before the event: the
    # check reads from the line of the search mark, scrollback too.
    waiting = s.start_call('wait', pane=pane, until='text', pattern='^TOKEN-42$', timeout_ms=15000)
    s.tool('send', pane=pane, paste="printf 'TOK''EN-%d\\n' 42; i=0; while [ $i -lt 200 ]; do echo filler$i; "
           "i=$((i+1)); done", keys=['enter'])
    result = s.result(waiting, 20)
    check(not result.get('isError') and result['structuredContent']['match'] == 'TOKEN-42', result)
    check(result['structuredContent']['status'] == 'matched', result)
    s.at_prompt(pane)
    # A prompt with no line end after it.
    waiting = s.start_call('wait', pane=pane, until='text', pattern='ask> ?$', timeout_ms=15000)
    s.tool('send', pane=pane, paste="read -r -p 'as''k> ' answer; echo got-$answer", keys=['enter'])
    result = s.result(waiting, 20)
    check(result['structuredContent']['match'].startswith('ask>'), result)
    # The wait goes first: it reads the text from the line of the cursor at its start.
    waiting = s.start_call('wait', pane=pane, until='text', pattern='got-yes', timeout_ms=10000)
    s.tool('send', pane=pane, text='yes\r')
    check(s.result(waiting, 15)['structuredContent']['matched'], 'no got-yes')
    s.at_prompt(pane)
    # The timeout of a pattern that never shows.
    result = s.tool('wait', pane=pane, until='text', pattern='never-there', timeout_ms=1000)
    check(result['status'] == 'running' and not result['matched'], result)
    # quiet: no new text for that long, after the last tick, which comes 0.9 s after the first.
    s.tool('send', pane=pane, paste="for i in 1 2 3 4; do sleep 0.3; echo t''ick$i; done", keys=['enter'])
    start = time.monotonic()
    result = s.tool('wait', pane=pane, until='quiet', quiet_ms=800, timeout_ms=10000)
    check(result['status'] == 'quiet' and 'tick4' in result['output'], result)
    check(time.monotonic() - start >= 1.7, (time.monotonic() - start, result))
    text = s.error('wait', pane=pane, until='text', pattern='(unclosed')
    check('regular expression' in text, text)


@case
def gdb_prompt(home):
    gdb = shutil.which('gdb')
    if not gdb:
        raise Skip('gdb is not in PATH')
    s = home.server()
    pane = s.first_pane()
    s.at_prompt(pane)
    waiting = s.start_call('wait', pane=pane, until='text', pattern=r'\(gdb\) ?$', timeout_ms=30000)
    s.tool('send', pane=pane, paste=f'{gdb} -q -nx', keys=['enter'])
    result = s.result(waiting, 35)
    check(not result.get('isError') and result['structuredContent']['matched'], result)
    s.tool('send', pane=pane, paste='quit', keys=['enter'])
    result = s.tool('wait', pane=pane, until='finished', timeout_ms=15000)
    check(result['status'] == 'finished', result)


@case
def wait_after_send(home):
    """A prompt that ends with a space, as gdb's and Python's do, and the next prompt after a send: not the prompt
    that the input answered, which the line in progress still shows until the program echoes the input."""
    s = home.server()
    pane = s.tool('open_pane', where='tab', argv=[
        home.bash, '--norc', '-c', 'while :; do printf "> "; IFS= read -r l || exit; sleep 1; echo "got $l"; done'])['pane']
    # Its first prompt, with the space before the cursor.
    result = s.tool('wait', pane=pane, until='text', pattern='^> $', timeout_ms=10000)
    check(result['status'] == 'matched' and result['match'] == '> ', result)
    # The input goes, and the answer comes a second later, then the next prompt.
    s.tool('send', pane=pane, text='x\r')
    start = time.monotonic()
    result = s.tool('wait', pane=pane, until='text', pattern='^> $', timeout_ms=10000)
    check(result['status'] == 'matched' and 'got x' in result['output'], result)
    check(time.monotonic() - start >= 0.8, (time.monotonic() - start, result))
    # A wait that began before the send reads what the input makes. The next
    # wait reads from its own start: the old answer does not match again.
    waiting = s.start_call('wait', pane=pane, until='text', pattern='^got y$', timeout_ms=10000)
    s.tool('send', pane=pane, text='y\r')
    check(s.result(waiting, 15)['structuredContent']['matched'], 'no got y')
    s.tool('wait', pane=pane, until='text', pattern='^> $', timeout_ms=10000)
    result = s.tool('wait', pane=pane, until='text', pattern='^got y$', timeout_ms=1500)
    check(result['status'] == 'running' and not result['matched'], result)
    check(result['notes'] == [], result)


@case
def fish_holds_an_incomplete_command(home):
    """Fish keeps an incomplete command in its editor and starts nothing: run says so in notes, which a client
    that reads only the structured content reads too."""
    fish = shutil.which('fish')
    if not fish:
        raise Skip('fish is not in PATH')
    home.settings(shell=fish)
    try:
        s = home.server()
    finally:
        home.settings()
    pane = s.first_pane()
    s.at_prompt(pane)
    result = s.tool('run', pane=pane, command='echo "open quote', timeout_ms=2000)
    check(result['status'] == 'running', result)
    check(len(result['notes']) == 1 and 'incomplete command' in result['notes'][0], result)
    s.tool('send', pane=pane, keys=['ctrl+c'])
    s.at_prompt(pane)
    result = s.tool('run', pane=pane, command='echo done', timeout_ms=10000)
    check(result['status'] == 'finished' and result['exit_code'] == 0 and result['notes'] == [], result)


@case
def flood(home):
    """wait until text during 50 MB of output: each check reads only what is new, and the server answers meanwhile."""
    s = home.server()
    first = s.first_pane()
    other = s.tool('open_pane', where='tab')['pane']
    s.at_prompt(first)
    s.at_prompt(other)
    # The flood runs until the test has its reads, so that their count does
    # not depend on how fast the machine prints: at least 700,000 lines, and
    # then on until the stop file appears.
    stop = home.root / 'stop-flood'
    waiting = s.start_call('wait', pane=first, until='text', pattern='END-OF-FLOOD', timeout_ms=120000)
    s.tool('send', pane=first, paste="n=0; while [ $n -lt 35 ] || [ ! -e " + str(stop) + " ]; do yes "
           "0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz | head -n 20000; "
           "n=$((n+1)); done; echo END-OF-''FLOOD", keys=['enter'])
    start = time.monotonic()
    slowest = 0.0
    # The reads that the server answered while the wait was still open: one
    # that waited for the flood would come after it.
    during = 0
    result = None
    while result is None:
        asked = time.monotonic()
        s.tool('read', pane=other)
        slowest = max(slowest, time.monotonic() - asked)
        if waiting not in s.replies:
            during += 1
        if during >= 3:
            stop.touch()
        try:
            result = s.result(waiting, 0.3)
        except Timeout:
            check(time.monotonic() - start < 120,
                  f'the flood did not end: {during} reads in another pane came during it')
    check(not result.get('isError') and result['structuredContent']['matched'], result)
    check(during >= 3, f'only {during} reads in another pane came during the flood')
    print(f'  flood: {time.monotonic() - start:.1f} s, {during} reads during it, slowest {slowest:.2f} s')


@case
def progress(home):
    s = home.server()
    pane = s.first_pane()
    s.at_prompt(pane)
    long_line = 'x' * 500
    id = s.start_call('run', progress='p1', pane=pane,
                      command=f'for i in 1 2 3 4 5; do echo step$i {long_line}; sleep 0.3; done')
    result = s.result(id, 30)
    check(result['structuredContent']['exit_code'] == 0, result)
    values = [n['params'] for n in s.notifications
              if n.get('method') == 'notifications/progress' and n['params'].get('progressToken') == 'p1']
    check(len(values) >= 2, s.notifications)
    check(all(b['progress'] > a['progress'] for a, b in zip(values, values[1:])), values)
    check(all(len(v.get('message', '')) <= 200 for v in values), values)
    check(any(v.get('message') for v in values), values)


@case
def concurrent_runs(home):
    s = home.server()
    first = s.first_pane()
    second = s.tool('open_pane', where='tab')['pane']
    third = s.tool('open_pane', where='tab')['pane']
    for pane in (first, second, third):
        s.at_prompt(pane)
    a = s.start_call('run', pane=first, command='sleep 3; echo A-done')
    b = s.start_call('run', pane=second, command='sleep 3; echo B-done')
    # The server answers in the order in which the calls end: a read that
    # waited for a run would come after its answer.
    s.tool('read', pane=third)
    check(a not in s.replies and b not in s.replies, 'a read waited for the runs')
    check('already' in s.error('run', pane=first, command='true'), 'a second run in a busy pane')
    check(a not in s.replies, 'the run ended before the refusal of a second one')
    for id, word in ((a, 'A-done'), (b, 'B-done')):
        result = s.result(id, 15)['structuredContent']
        check(result['status'] == 'finished' and word in result['output'], result)


@case
def refusals(home):
    s = home.server()
    pane = s.first_pane()
    s.at_prompt(pane)
    s.tool('send', pane=pane, text='abc')
    wait_for(lambda: s.panes()[pane]['input'], 5, 'the typed text does not show as input')
    text = s.error('run', pane=pane, command='true')
    check('typed' in text, text)
    s.tool('send', pane=pane, keys=['ctrl+u'])
    s.at_prompt(pane)
    sh = shutil.which('sh') or '/bin/sh'
    plain = s.tool('open_pane', where='tab', argv=[sh, '-c', 'echo plain-started; exec sleep 600'])['pane']
    # The program runs: the refusal is of a pane with no shell integration,
    # not of one whose shell has not started.
    wait_for(lambda: 'plain-started' in s.tool('read', pane=plain)['text'], 10, 'the plain pane did not start')
    text = s.error('run', pane=plain, command='true')
    check('no shell integration' in text, text)
    text = s.error('open_pane', where='split')
    check('pane' in text, text)
    check('send needs' in s.error('send', pane=pane), 'send with nothing')
    check('no pane' in s.error('run', pane=999999, command='true'), 'a pane that does not exist')


@case
def sessions_and_adopt(home):
    one = home.server()
    two = home.server()
    check(one.session != two.session and one.session.startswith('mcp-'), (one.session, two.session))
    listed = home.sessions()
    check(listed[one.session]['agent'] and listed[one.session]['bound'] == 1, listed)
    # A session takes Control.allowAdopt when it starts: off by default, so no server adopts it.
    made = home.mux('new', '--detach', '--name', 'closed')
    check(made.returncode == 0, made.stderr)
    shut = home.server('--session', 'closed', '--adopt', check_start=False)
    check(shut.process.wait(15) == 1 and 'Control.allowAdopt' in shut.stderr(), shut.stderr())
    check(not home.sessions()['closed']['agent'], home.sessions())
    home.daemon('killSession', ['closed'])
    home.settings(control={'allowAdopt': True})
    made = home.mux('new', '--detach', '--name', 'plain')
    check(made.returncode == 0, made.stderr)
    refused = home.server('--session', 'plain', check_start=False)
    check(refused.process.wait(15) == 1 and '--adopt' in refused.stderr(), refused.stderr())
    adopted = home.server('--session', 'plain', '--adopt')
    check(adopted.session == 'plain' and home.sessions()['plain']['agent'], home.sessions())
    check(home.sessions()['plain']['bound'] == 0, home.sessions())
    adopted.close()
    home.settle()
    check('plain' in home.sessions(), 'an adopted session ended with the server')
    home.daemon('killSession', ['plain'])


@case
def session_ends_with_the_server(home):
    for how in ('stdin', signal.SIGTERM, signal.SIGKILL):
        s = home.server()
        if how == 'stdin':
            check(s.close() == 0, s.stderr())
        else:
            s.kill(how)
        wait_for(lambda: s.session not in home.sessions(), 5, f'the session outlived the server ({how})')
    home.settings(endSessionOnExit=False)
    s = home.server()
    check(home.sessions()[s.session]['bound'] == 0, home.sessions())
    s.close()
    home.settle()
    check(s.session in home.sessions(), 'the session ended with Mcp.endSessionOnExit false')
    home.daemon('killSession', [s.session])
    home.settings()


@case
def two_servers_on_one_session(home):
    for how in ('stdin', signal.SIGKILL):
        first = home.server()
        second = home.server('--session', first.session)
        check(home.sessions()[first.session]['bound'] == 2, home.sessions())
        if how == 'stdin':
            first.close()
        else:
            first.kill(how)
        # The daemon read the end of the first server's connection.
        wait_for(lambda: home.sessions().get(first.session, {}).get('bound') != 2, 5,
                 f'the daemon did not see the first server end ({how})')
        check(home.sessions().get(first.session, {}).get('bound') == 1,
              f'the session ended with the first server ({how}): {home.sessions()}')
        check(second.tool('list_panes')['session'] == first.session, 'the second server lost the session')
        second.close()
        wait_for(lambda: first.session not in home.sessions(), 5, 'the session outlived its last server')


@case
def kill_session_during_a_run(home):
    s = home.server()
    pane = s.first_pane()
    s.at_prompt(pane)
    id = s.start_call('run', pane=pane, command='sleep 30')
    time.sleep(0.5)
    home.daemon('killSession', [s.session])
    result = s.result(id, 10)
    check(result.get('isError'), result)
    text = s.error('list_panes')
    check('ended' in text, text)


@case
def close_the_last_pane(home):
    s = home.server()
    pane = s.first_pane()
    other = s.tool('open_pane', where='tab')['pane']
    s.tool('close_pane', pane=other)
    text = s.error('close_pane', pane=pane)
    check('end_session' in text, text)
    check(s.session in home.sessions(), 'a refused close ended the session')
    text = s.tool('close_pane', pane=pane, end_session=True)
    check('ended' in text, text)
    wait_for(lambda: s.session not in home.sessions(), 5, 'the session outlived its last pane')


@case
def close_two_panes_at_once(home):
    """Two closes at once of the two panes of a session: the daemon refuses the one that comes last."""
    s = home.server()
    first = s.first_pane()
    second = s.tool('open_pane', where='tab')['pane']
    calls = [s.start_call('close_pane', pane=first), s.start_call('close_pane', pane=second)]
    results = [s.result(id) for id in calls]
    refused = [r for r in results if r.get('isError')]
    check(len(refused) == 1 and 'end_session' in refused[0]['content'][0]['text'], results)
    check(s.session in home.sessions() and len(s.panes()) == 1, home.sessions())


@case
def run_in_a_new_pane(home):
    """A run in a pane that the server just opened waits for the first prompt of its shell."""
    s = home.server()
    pane = s.tool('open_pane', where='tab')['pane']
    result = s.tool('run', pane=pane, command='echo fi""rst')
    check(result['status'] == 'finished' and 'first' in result['output'], result)


@case
def control_off(home):
    """With Control.enabled off, no tool could reach a new session: the server makes none."""
    home.settings(control={'enabled': False})
    s = home.server(check_start=False)
    check(s.process.wait(15) == 1 and 'Control.enabled' in s.stderr(), s.stderr())
    check(not home.sessions_if_any(), home.sessions_if_any())


@case
def splits_without_a_window(home):
    s = home.server()
    pane = s.first_pane()
    for _ in range(64):
        result = s.call('open_pane', where='split', pane=pane, direction='right')
        if result.get('isError'):
            text = result['content'][0]['text']
            check('open a tab' in text, text)
            return
        pane = result['structuredContent']['pane']
    raise Failure('64 splits found room')


@case
def screenshot_without_a_window(home):
    s = home.server()
    text = s.error('screenshot', pane=s.first_pane())
    check('attach' in text and s.session in text, text)


@case
def zsh_paste(home):
    zsh = shutil.which('zsh')
    if not zsh:
        raise Skip('zsh is not in PATH')
    home.settings(shell=zsh)
    try:
        s = home.server()
        pane = s.first_pane()
        s.at_prompt(pane)
        result = s.tool('run', pane=pane, command='echo o""ne\n(exit 4)')
        check(result['exit_codes'] == [4] and result['exit_code'] == 4, result)
    finally:
        home.settings()


def write_project(home, name, document):
    directory = home.app / 'projects'
    directory.mkdir(exist_ok=True)
    (directory / f'{name}.json').write_text(json.dumps(document))


def project_pane(label, cwd, command=()):
    return {'pane': {'id': label, 'cwd': str(cwd), 'title': '', 'command': list(command)}}


@case
def projects(home):
    """open_project opens a saved project into the agent's own session, and save_project saves the session back."""
    work = home.home / 'work'
    work.mkdir(exist_ok=True)
    # sh through PATH: the project looks its programs up as a pane does.
    marker = ['sh', '-c', 'echo project-c; exec sleep 600']
    write_project(home, 'dev', {
        'version': 1, 'active_tab': 't2', 'session': True,
        'tabs': [
            {'id': 't1', 'title': 'Edit', 'active_pane': 'b',
             'tree': {'direction': 'right', 'ratio': 0.3,
                      'first': project_pane('a', home.home),
                      'second': {'direction': 'down', 'ratio': 0.5,
                                 'first': project_pane('b', work), 'second': project_pane('d', work)}}},
            {'id': 't2', 'title': 'Run', 'active_pane': 'c', 'tree': project_pane('c', work, marker)},
        ]})
    s = home.server()
    first = s.first_pane()
    opened = s.tool('open_project', name='dev')
    ids = opened['panes']
    check(sorted(ids) == ['a', 'b', 'c', 'd'] and opened['name'] == 'dev', opened)
    panes = s.panes()
    check(len(panes) == 5 and all(ids[label] in panes for label in ids), panes)
    # The tabs of the project come after the session's own, in their order,
    # with the splits of the first one in one tab.
    tab = {label: panes[ids[label]]['tab'] for label in ids}
    check(tab['a'] == tab['b'] == tab['d'] and tab['c'] == tab['a'] + 1 and tab['a'] > panes[first]['tab'], tab)
    wait_for(lambda: 'project-c' in s.tool('read', pane=ids['c'])['text'], 10, 'pane c did not run its command')
    # No session of the project's name: the project opened here.
    check('dev' not in home.sessions(), home.sessions())
    text = s.error('open_project', name='missing')
    check('dev' in text, text)

    saved = s.tool('save_project', name='saved')
    path = Path(saved['path'])
    check(path == home.app / 'projects' / 'saved.json' and saved['panes'] == 5, saved)
    document = json.loads(path.read_text())
    check(document['session'] is True and len(document['tabs']) == 3, document)
    split = document['tabs'][1]['tree']
    check(split['direction'] == 'right' and split['second']['direction'] == 'down', split)
    leaves = {}

    def collect(node):
        if 'pane' in node:
            leaves[int(node['pane']['id'])] = node['pane']
        else:
            collect(node['first'])
            collect(node['second'])
    for t in document['tabs']:
        collect(t['tree'])
    check(leaves[ids['c']]['command'] == marker and leaves[ids['a']]['command'] == [], leaves)
    check(leaves[ids['b']]['cwd'] == str(work), leaves)
    # The active tab of the project has the focus, and is the active tab of the save.
    check(document['tabs'][int(document['active_tab']) - 1]['active_pane'] == str(ids['c']), document)
    # A saved project stays without overwrite.
    text = s.error('save_project', name='saved')
    check('overwrite' in text, text)
    check(s.tool('save_project', name='saved', overwrite=True)['name'] == 'saved', 'the overwrite failed')
    check(s.error('save_project', name='no/name'), 'a name with a slash saved')
    # The saved project opens again, in the session of another server.
    other = home.server()
    again = other.tool('open_project', name='saved')
    check(len(again['panes']) == 5 and len(other.panes()) == 6, (again, other.panes()))


@case
def script_commands_without_a_window(home):
    s = home.server()
    text = s.error('list_script_commands')
    check('afterglow mux attach ' + s.session in text, text)
    text = s.error('run_script_command', pane=s.first_pane(), script='x', command='y')
    check('afterglow mux attach ' + s.session in text, text)


SCRIPT = """local script = {}
script.metadata = {
    commands = {
        echo = { summary = "Echo the arguments and the input", stdin = true },
        plain = { summary = "Read no input" },
    },
}
function script.run_command(command, ev)
    if command == "echo" then
        return {
            afterglow.output(ev, table.concat(ev.args, " ") .. "|" .. (ev.stdin or "nil")),
            afterglow.output_error(ev, "to stderr"),
            afterglow.exit(ev, 3),
        }
    end
    return afterglow.exit(ev, 0)
end
return script
"""


@window_case
def script_command_through_a_window(home):
    """A window of the session in a headless KWin of the run's own, with a script of the run's config, runs the
    command. Never the user's display: KWin and the window get the run's runtime directory and a bus of their own."""
    for program in ('kwin_wayland', 'dbus-run-session'):
        if not shutil.which(program):
            raise Skip(f'{program} is not in PATH')
    scripts = home.app / 'scripts'
    scripts.mkdir(exist_ok=True)
    (scripts / 'mcptest.luau').write_text(SCRIPT)
    (home.config / 'kwinrc').write_text(KWINRC)
    s = home.server()
    pane = s.first_pane()
    display = f'aglow-mcp-{os.getpid()}'
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
        window = subprocess.Popen(['dbus-run-session', '--', home.binary, 'mux', 'attach', s.session],
                                  env=window_env, stdout=home.log_file('window'), stderr=subprocess.STDOUT,
                                  start_new_session=True)
        wait_for(lambda: home.sessions().get(s.session, {}).get('attached') or window.poll() is not None, 30,
                 'no window showed the session')
        if window.poll() is not None:
            tail = (home.root / 'window.log').read_text(errors='replace')[-600:]
            # No Vulkan device in a container: the window cannot draw. Any
            # other end at the start is a failure.
            if 'failed to create the Vulkan' in tail:
                raise Skip(f'the window has no Vulkan device: {tail}')
            raise Failure(f'the window of the session exited at its start: {tail}')

        def listed():
            result = s.call('list_script_commands')
            if result.get('isError'):
                return None
            found = {(c['script'], c['command']): c for c in result['structuredContent']['commands']}
            return found if ('mcptest', 'echo') in found else None
        commands = wait_for(listed, 20, 'the window lists no command of the script')
        check(commands[('mcptest', 'echo')]['stdin'] is True and not commands[('mcptest', 'plain')]['stdin'],
              commands)
        result = s.tool('run_script_command', pane=pane, script='mcptest', command='echo', args=['a', 'b c'],
                        stdin='in')
        check(result == {'exit_code': 3, 'stdout': 'a b c|in', 'stderr': 'to stderr', 'truncated': False}, result)
        result = s.tool('run_script_command', pane=pane, script='mcptest', command='echo')
        check(result['stdout'] == '|nil', result)
        text = s.error('run_script_command', pane=pane, script='mcptest', command='plain', stdin='x')
        check('standard input' in text, text)
        text = s.error('run_script_command', pane=pane, script='mcptest', command='none')
        check('list_script_commands' in text, text)
        # A pane of another session is no pane of this window.
        other = home.server()
        text = s.error('run_script_command', pane=other.first_pane(), script='mcptest', command='plain')
        check('shows in its window' in text, text)
    finally:
        for process in (window, kwin):
            if process and process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


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
    failures = 0
    ran = 0
    skipped = 0
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
                for server in home.servers:
                    server.kill()
                home.servers.clear()
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
