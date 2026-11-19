#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drives `afterglow mcp` as an MCP client does: one JSON-RPC message per line on its standard input and output.

Each server runs with a private XDG_RUNTIME_DIR, XDG_STATE_HOME, XDG_CONFIG_HOME and HOME under /tmp, and only the
variables of an allow list, so that no server reaches the user's mux daemon, Afterglow, bus or shell files. The
servers of a run share one private daemon, which the first one starts. Bash is the shell of the session, with the
shell integration of the source tree (an afterglow in a build tree reads it) and an rc file that sets the prompt that
the checks wait for. Bash is required. The zsh and gdb cases skip with a reason when the program is not in PATH. At the
end, and on a failure, the run ends every server, session and daemon that it started.

Usage: mcp_session.py AFTERGLOW [CASE...]
"""
import json
import os
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

VERSION = '2026-07-28'
META = {'io.modelcontextprotocol/protocolVersion': VERSION, 'io.modelcontextprotocol/clientCapabilities': {}}
PROMPT = 'mcp$ '
ALLOWED = {'PATH', 'LANG', 'LANGUAGE', 'TZ', 'USER', 'LOGNAME', 'LD_LIBRARY_PATH'}


class Failure(Exception):
    pass


class Skip(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


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
        self.root = Path(tempfile.mkdtemp(prefix='aglow-mcp-', dir='/tmp'))
        self.runtime = self.root / 'run'
        self.home = self.root / 'home'
        self.config = self.root / 'config'
        for directory in (self.runtime, self.home, self.config / 'afterglow', self.root / 'state'):
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

    def settings(self, shell=None, **mcp):
        """The config file that the next server reads, once, when it starts."""
        config = {'Terminal': {'shell': shell or self.bash, 'confirmClose': False},
                  'Control': {'eventInterval': 100}, 'Mcp': mcp}
        (self.config / 'afterglow' / 'config.json').write_text(json.dumps(config))

    def server(self, *args, check_start=True):
        server = Server(self, list(args))
        self.servers.append(server)
        if check_start:
            server.wait_started()
        return server

    def daemon(self, fn, args=(), target='mux'):
        """One call of the daemon's mux target, as a raw phosphor-ipc client."""
        path = self.runtime / 'aglow-mux' / 'socket'
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(5)
            try:
                s.connect(str(path))
            except OSError:
                return None
            s.sendall((json.dumps({'type': 'call', 'id': 1, 'target': target, 'fn': fn, 'args': list(args)})
                       + '\n').encode())
            data = b''
            while not data.endswith(b'\n'):
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
        reply = json.loads(data.splitlines()[0]) if data else {}
        return reply

    def sessions(self):
        reply = self.daemon('listSessions')
        return {s['name']: s for s in reply.get('result', [])} if reply else {}

    def mux(self, *args):
        return subprocess.run([self.binary, 'mux', *args], env=self.env(), capture_output=True, text=True, timeout=30)

    def our_processes(self):
        """The processes whose environment names this run's runtime directory: the daemon and the panes."""
        marker = f'XDG_RUNTIME_DIR={self.runtime}'.encode()
        found = []
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit() or int(entry.name) == os.getpid():
                continue
            try:
                if marker in (entry / 'environ').read_bytes().split(b'\0'):
                    found.append(int(entry.name))
            except OSError:
                continue
        return found

    def close(self):
        for server in self.servers:
            server.kill()
        for name in self.sessions():
            self.daemon('killSession', [name])
        # The daemon ends with its last session. One that does not, and every
        # pane of this run, ends here.
        try:
            wait_for(lambda: not self.our_processes(), 5)
        except Failure:
            for pid in self.our_processes():
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
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
                raise Failure(f'no message from the server; its stderr:\n{self.stderr()}')
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

    def start_call(self, name, progress=None, **arguments):
        extra = {'progressToken': progress} if progress is not None else {}
        return self.request('tools/call', {'name': name, 'arguments': arguments}, **extra)

    def result(self, id, timeout=30.0):
        reply = self.reply(id, timeout)
        check('result' in reply, f'a protocol error: {reply}')
        return reply['result']

    def call(self, name, timeout=30.0, **arguments):
        return self.result(self.start_call(name, **arguments), timeout)

    def tool(self, name, timeout=30.0, **arguments):
        """A call that must succeed: its structured content, or its text."""
        result = self.call(name, timeout, **arguments)
        check(not result.get('isError'), f'{name} {arguments} failed: {result["content"][0]["text"]}')
        return result.get('structuredContent', result['content'][0]['text'])

    def error(self, name, timeout=30.0, **arguments):
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
        if self.process.poll() is None:
            self.process.send_signal(sig)
            self.process.wait(10)
        self.log.close()


CASES = []


def case(function):
    CASES.append(function)
    return function


@case
def discover_and_tools(home):
    """One server/discover, a bad version and the tool list end to end: the wiring, not the library."""
    s = home.server()
    reply = s.reply(s.request('server/discover'))
    discover = reply['result']
    check(discover['supportedVersions'] == [VERSION], discover)
    check(s.session in discover['instructions'] and 'list_panes' in discover['instructions'], discover)
    check(discover['_meta']['io.modelcontextprotocol/serverInfo']['name'] == 'afterglow', discover)
    check(discover['cacheScope'] == 'public', discover)
    bad = s.reply(s.request('tools/list', meta=dict(META, **{'io.modelcontextprotocol/protocolVersion': '2025-11-25'})))
    check(bad['error']['code'] == -32022 and bad['error']['data']['requested'] == '2025-11-25', bad)
    tools = s.reply(s.request('tools/list'))['result']['tools']
    names = [t['name'] for t in tools]
    check(names == ['list_panes', 'open_pane', 'run', 'wait', 'send', 'read', 'screenshot', 'close_pane'], names)
    expected = {'list_panes': (True, False), 'open_pane': (False, False), 'run': (False, True),
                'wait': (True, False), 'send': (False, True), 'read': (True, False),
                'screenshot': (True, False), 'close_pane': (False, True)}
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
    # The pane is this server's until the run ends.
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
    # quiet: no new text for that long.
    s.tool('send', pane=pane, paste='for i in 1 2 3; do echo tick; sleep 0.2; done', keys=['enter'])
    start = time.monotonic()
    result = s.tool('wait', pane=pane, until='quiet', quiet_ms=800, timeout_ms=10000)
    check(result['status'] == 'quiet' and time.monotonic() - start >= 0.8, result)
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
def flood(home):
    """wait until text during 50 MB of output: each check reads only what is new, and the server answers meanwhile."""
    s = home.server()
    first = s.first_pane()
    other = s.tool('open_pane', where='tab')['pane']
    s.at_prompt(first)
    s.at_prompt(other)
    waiting = s.start_call('wait', pane=first, until='text', pattern='END-OF-FLOOD', timeout_ms=120000)
    s.tool('send', pane=first, paste="yes 0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz "
           "| head -n 700000; echo END-OF-''FLOOD", keys=['enter'])
    start = time.monotonic()
    slowest = 0.0
    result = None
    while result is None:
        asked = time.monotonic()
        s.tool('read', pane=other)
        slowest = max(slowest, time.monotonic() - asked)
        try:
            result = s.result(waiting, 0.3)
        except Failure:
            check(time.monotonic() - start < 120, 'the flood did not end')
    check(not result.get('isError') and result['structuredContent']['matched'], result)
    check(slowest < 3, f'a read in another pane took {slowest:.1f} s during the flood')
    print(f'  flood: {time.monotonic() - start:.1f} s, slowest read {slowest:.2f} s')


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
    a = s.start_call('run', pane=first, command='sleep 2; echo A-done')
    b = s.start_call('run', pane=second, command='sleep 2; echo B-done')
    time.sleep(0.3)
    asked = time.monotonic()
    s.tool('read', pane=third)
    check(time.monotonic() - asked < 1.5, 'a read waited for the runs')
    check('already' in s.error('run', pane=first, command='true'), 'a second run in a busy pane')
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
    plain = s.tool('open_pane', where='tab', argv=[sh])['pane']
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
    made = home.mux('new', '--detach', '--name', 'plain')
    check(made.returncode == 0, made.stderr)
    refused = home.server('--session', 'plain', check_start=False)
    check(refused.process.wait(15) == 1 and '--adopt' in refused.stderr(), refused.stderr())
    adopted = home.server('--session', 'plain', '--adopt')
    check(adopted.session == 'plain' and home.sessions()['plain']['agent'], home.sessions())
    check(home.sessions()['plain']['bound'] == 0, home.sessions())
    adopted.close()
    time.sleep(0.5)
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
    s.close()
    time.sleep(0.5)
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
        time.sleep(0.5)
        check(first.session in home.sessions(), f'the session ended with the first server ({how})')
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
    s.tool('close_pane', pane=pane, end_session=True)
    wait_for(lambda: s.session not in home.sessions(), 5, 'the session outlived its last pane')


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


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    binary = str(Path(sys.argv[1]).resolve())
    chosen = sys.argv[2:]
    home = Home(binary)
    failures = 0
    try:
        for function in CASES:
            if chosen and function.__name__ not in chosen:
                continue
            started = time.monotonic()
            try:
                function(home)
                print(f'PASS {function.__name__} ({time.monotonic() - started:.1f} s)', flush=True)
            except Skip as skip:
                print(f'SKIP {function.__name__}: {skip}', flush=True)
            except Exception:
                failures += 1
                print(f'FAIL {function.__name__}:\n{traceback.format_exc()}', flush=True)
            finally:
                for server in home.servers:
                    server.kill()
                home.servers.clear()
                home.settings()
                # Each case starts with no session of the ones before.
                for name in home.sessions():
                    home.daemon('killSession', [name])
    finally:
        home.close()
    print(f'{failures} failed' if failures else 'all passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
