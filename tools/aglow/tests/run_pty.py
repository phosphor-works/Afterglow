#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""aglow run on a remote host: the query for the socket of the ssh session (OSC 24569).

Usage: run_pty.py AGLOW

This script plays the terminal in a pseudo-terminal that it makes, and a
terminal socket (the "remote" target) on a Unix socket of its own. It checks:

- the DA1 guard: a terminal that is not Afterglow, one that never answers,
  and a reply that comes in parts;
- the replies: a path that is not on this host, a reason in place of a
  path, and a run through the socket, also with an AFTERGLOW_SOCKET that
  names no socket here;
- a pane's own socket on this host (AFTERGLOW_SOCKET), which needs no query;
- no query without the terminal: standard output in a pipe, a background
  process group, no controlling terminal;
- the terminal's modes after a signal and after Ctrl+C, also with SIGINT
  ignored, and no late reply in the shell's input;
- aglow remote-prune.

tmux_pty.py checks the query inside tmux. aglow never sees an
AFTERGLOW_SOCKET or a TMUX of the session that runs the test.
"""
import json
import os
import pty
import select
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

QUERY = b"\x1b]24569;q=remote\x1b\\"
GUARD = b"\x1b[?62;22c"
# What aglow says when it did not ask the terminal.
NOT_ASKED = b"only in the foreground, with standard output on the terminal"
# A process group of its own, out of the terminal's foreground: python -c HELPER AGLOW ARGS...
BACKGROUND = ("import os, sys\n"
              "pid = os.fork()\n"
              "if pid == 0:\n"
              "    os.setpgid(0, 0)\n"
              "    os.execv(sys.argv[1], sys.argv[1:])\n"
              "sys.exit(os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]) & 0xff)\n")


def check(condition, *detail):
    """Raises AssertionError when condition is false. Not assert: python -O must not turn the checks off."""
    if not condition:
        raise AssertionError(*detail)


class FakeTerminalSocket:
    """A phosphor-ipc server with the target "remote", as Afterglow forwards it."""

    def __init__(self, path, target="remote"):
        self.path = path
        self.target = target
        self.calls = []
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(path)
        self.server.listen(4)
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                connection, _ = self.server.accept()
            except OSError:
                return
            threading.Thread(target=self.talk, args=(connection,), daemon=True).start()

    def talk(self, connection):
        buffer = b""
        with connection:
            while True:
                data = connection.recv(65536)
                if not data:
                    return
                buffer += data
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    for frame in self.answer(json.loads(line)):
                        connection.sendall(json.dumps(frame).encode() + b"\n")

    def answer(self, call):
        self.calls.append(call)
        if call.get("target") != self.target:
            return [{"type": "error", "id": call["id"], "code": "NO_SUCH_TARGET", "message": "no such target"}]
        if call["fn"] == "list":
            return [{"type": "reply", "id": call["id"], "result": [
                {"script": "far", "command": "there", "summary": "From remote hosts", "usage": "", "palette": "",
                 "stdin": False, "remote": True}]}]
        if call["fn"] == "run":
            run, _script, command, args, _cwd, pane = call["args"]
            text = f"{command} {' '.join(args)} pane={pane}\n"
            return [{"type": "event", "target": "remote", "signal": "stdout", "args": [run, text]},
                    {"type": "reply", "id": call["id"], "result": {"exit": 3}}]
        return [{"type": "reply", "id": call["id"], "result": None}]

    def close(self):
        self.server.close()


def environment(runtime, extra=None):
    """The environment of aglow: no socket of a pane and no tmux of the session."""
    env = {k: v for k, v in os.environ.items()
           if k not in ("AFTERGLOW_SOCKET", "AFTERGLOW_PANE_ID", "TMUX", "TMUX_PANE", "WAYLAND_DISPLAY")}
    env["XDG_RUNTIME_DIR"] = runtime
    env["TERM"] = "xterm-256color"
    env.update(extra or {})
    return env


def play(argv, env, answer, timeout=20, also=()):
    """Runs ARGV in a pseudo-terminal. ANSWER(written) gives the bytes that the terminal sends for what the
    program wrote so far, once for each query: bytes, or a list of parts that come 0.2 seconds apart. ALSO
    holds (sequence, answer) pairs: each time the program writes the sequence, the terminal sends the answer,
    whichever read the sequence came in. Returns (exit code, everything written, seconds taken)."""
    started = time.monotonic()
    pid, master = pty.fork()
    if pid == 0:
        try:
            os.execve(argv[0], argv, env)
        finally:
            os._exit(127)
    written = b""
    answered = 0
    answered_also = [0] * len(also)
    deadline = started + timeout
    code = None
    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.02)
            if ready:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    data = b""
                written += data
            while written.count(QUERY) > answered:
                reply = answer(written)
                answered += 1
                for i, part in enumerate(reply if isinstance(reply, list) else [reply]):
                    if i:
                        time.sleep(0.2)
                    if part:
                        os.write(master, part)
            for i, (sequence, reply) in enumerate(also):
                while written.count(sequence) > answered_also[i]:
                    answered_also[i] += 1
                    os.write(master, reply)
            found, status = os.waitpid(pid, os.WNOHANG)
            if found:
                code = os.waitstatus_to_exitcode(status)
                # What the program wrote before it ended.
                while select.select([master], [], [], 0.05)[0]:
                    try:
                        data = os.read(master, 65536)
                    except OSError:
                        break
                    if not data:
                        break
                    written += data
                break
    finally:
        if code is None:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
        os.close(master)
    return code, written, time.monotonic() - started


def sh(script):
    """A shell in the pseudo-terminal that runs SCRIPT."""
    return ["/bin/sh", "-c", script]


def modes_back(written):
    """Whether stty -a, after aglow, shows the terminal's modes of before: echo and line editing."""
    modes = written[written.rfind(b"speed"):].replace(b";", b" ").split()
    return all(mode in modes for mode in (b"icanon", b"echo", b"isig", b"iexten", b"-noflsh"))


def direct_cases(aglow, scratch):
    runtime = os.path.join(scratch, "run")
    os.mkdir(runtime, 0o700)
    server = FakeTerminalSocket(os.path.join(scratch, "remote.sock"))
    argv = [aglow, "run", "far", "there", "a", "b"]
    path_reply = b"\x1b]24569;path=" + server.path.encode() + b"\x1b\\"
    try:
        # A terminal that is not Afterglow answers DA1 alone: no wait for the guard's time.
        code, written, took = play(argv, environment(runtime), lambda _: GUARD)
        check(written.startswith(QUERY + b"\x1b[c"), written)
        check(code == 1, code, written)
        check(b"no running Afterglow on this display" in written, written)
        check(took < 1.9, "aglow waited for the guard", took)

        # A terminal that never answers: the guard's time, then the local lookup, with the modes back.
        code, written, took = play(sh(f"{shlex.quote(aglow)} run far there; echo status=$?; stty -a"),
                                   environment(runtime), lambda _: None)
        check(b"status=1" in written and b"no running Afterglow on this display" in written, written)
        check(2.0 <= took < 6, took)
        check(modes_back(written), written)

        # A reply that comes in parts, and the guard after it.
        code, written, _ = play(argv, environment(runtime), lambda _: [path_reply[:9], path_reply[9:] + GUARD[:3], GUARD[3:]])
        check(code == 3 and b"there a b pane=0\r\n" in written, code, written)

        # The path of a session on another host: a second ssh hop.
        code, written, _ = play(argv, environment(runtime),
                                lambda _: b"\x1b]24569;path=/nowhere/0123456789abcdef.sock\x1b\\" + GUARD)
        check(code == 1, code, written)
        check(b"/nowhere/0123456789abcdef.sock, is on another host" in written, written)

        # Each reason in place of a path.
        for reason, text in ((b"forward", b"AllowStreamLocalForwarding"), (b"version", b"another protocol"),
                             (b"length", b"over 103 bytes"), (b"off", b"Terminal.sshRemoteCommands")):
            code, written, _ = play(argv, environment(runtime),
                                    lambda _, r=reason: b"\x1b]24569;error=" + r + b"\x1b\\" + GUARD)
            check(code == 1, reason, code, written)
            check(text in written, reason, written)

        # The socket of the session: the "remote" target, on the session's pane. The AFTERGLOW_SOCKET and
        # AFTERGLOW_PANE_ID of a pane on another host name no socket here: aglow asks, and the pane ID goes.
        code, written, _ = play(argv, environment(runtime, {"AFTERGLOW_SOCKET": os.path.join(scratch, "none.sock"),
                                                            "AFTERGLOW_PANE_ID": "12"}),
                                lambda _: path_reply + GUARD)
        check(code == 3, code, written)
        check(b"there a b pane=0\r\n" in written, written)
        check({call["target"] for call in server.calls} == {"remote"}, server.calls)

        # A socket without the remote target.
        other = FakeTerminalSocket(os.path.join(scratch, "other.sock"), target="commands")
        try:
            code, written, _ = play(argv, environment(runtime),
                                    lambda _: b"\x1b]24569;path=" + other.path.encode() + b"\x1b\\" + GUARD)
            check(code == 1 and b"has no target for remote commands" in written, code, written)
        finally:
            other.close()

        # A pane's own socket on this host needs no query.
        server.calls.clear()
        code, written, _ = play(argv, environment(runtime, {"AFTERGLOW_SOCKET": server.path}), lambda _: None)
        check(QUERY not in written, written)
        # The local target is "commands", which the fake socket does not have.
        check(code == 1 and b"takes no commands" in written, code, written)
    finally:
        server.close()


def unasked_cases(aglow, scratch):
    """No query where another reader may take the answer, or where the terminal would stop aglow."""
    runtime = os.path.join(scratch, "run")
    # Standard output in a pipe, whose reader may read the terminal too (less).
    code, written, _ = play(sh(f"{shlex.quote(aglow)} run far there | cat; echo status=$?"), environment(runtime),
                            lambda _: GUARD)
    check(QUERY not in written, written)
    check(b"no running Afterglow on this display" in written and NOT_ASKED in written, written)
    # A process group in the background, which SIGTTOU would stop with the terminal in its modes.
    code, written, took = play([sys.executable, "-I", "-c", BACKGROUND, aglow, "run", "far", "there"],
                               environment(runtime), lambda _: GUARD)
    check(code == 1, code, written)
    check(QUERY not in written and NOT_ASKED in written, written)
    check(took < 1.9, took)
    # A tmux that does not answer in time: the query would get no time to wait, and its answer would reach the
    # shell. aglow asks nothing, and the wait ends at the guard's time.
    fake = os.path.join(scratch, "fake-tmux")
    os.makedirs(fake, exist_ok=True)
    with open(os.path.join(fake, "tmux"), "w") as f:
        f.write("#!/bin/sh\nexec sleep 10\n")
    os.chmod(os.path.join(fake, "tmux"), 0o755)
    env = environment(runtime, {"TMUX": os.path.join(scratch, "no-tmux-server") + ",0,0",
                                "PATH": fake + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin")})
    code, written, took = play([aglow, "run", "far", "there"], env, lambda _: GUARD)
    check(code == 1 and b"no running Afterglow on this display" in written, code, written)
    check(b"24569" not in written, "the query went after tmux took the whole wait", written)
    check(took < 6, took)
    # tmux gets half of the guard's time. One that answers after that leaves the query too little time: aglow
    # asks nothing. One that answers early leaves the query the rest, and the status report ends the wait.
    for delay, asks in (("1.5", False), ("0.3", True)):
        with open(os.path.join(fake, "tmux"), "w") as f:
            f.write(f"#!/bin/sh\nsleep {delay}\necho on\n")
        code, written, took = play([aglow, "run", "far", "there"], env, lambda _: GUARD,
                                   also=[(b"\x1b[5n", b"\x1b[0n")])
        check(code == 1 and b"no running Afterglow on this display" in written, delay, code, written)
        check((b"24569" in written) == asks, delay, "the query went with too little time" if not asks
              else "no query after an early answer of tmux", written)
        check(took < 1.9 if asks else took < 6, delay, took)
    # No controlling terminal at all.
    result = subprocess.run([aglow, "run", "far", "there"], env=environment(runtime), stdin=subprocess.DEVNULL,
                            capture_output=True, start_new_session=True, timeout=20)
    check(result.returncode == 1 and NOT_ASKED in result.stderr, result)
    print("aglow run: no query without the foreground of the terminal", flush=True)


def signal_cases(aglow, scratch):
    """Signals and Ctrl+C while aglow waits for the answer: the terminal's modes come back."""
    runtime = os.path.join(scratch, "run")
    server = FakeTerminalSocket(os.path.join(scratch, "signal.sock"))
    path_reply = b"\x1b]24569;path=" + server.path.encode() + b"\x1b\\"
    pidfile = os.path.join(scratch, "aglow.pid")
    try:
        # SIGTERM while the terminal does not answer: aglow ends by it, after the modes are back. A job of sh
        # is in the foreground's process group, as sh has no job control.
        def sender(path, numbers):
            def send(_):
                for _ in range(100):
                    if os.path.exists(path) and open(path).read().strip():
                        break
                    time.sleep(0.02)
                for number in numbers:
                    os.kill(int(open(path).read()), number)
            return send

        # SIGTSTP and then SIGTERM: both act once the modes are back, and SIGTERM ends aglow. With only the
        # first signal kept, SIGTERM would be lost. The process group of sh here is orphaned, so the kernel
        # drops SIGTSTP and aglow does not stop.
        for name, numbers in (("term", [signal.SIGTERM]), ("stop-term", [signal.SIGTSTP, signal.SIGTERM])):
            path = f"{pidfile}.{name}"
            code, written, took = play(sh(f"{shlex.quote(aglow)} run far there & echo $! > {shlex.quote(path)};"
                                          " wait $!; echo status=$?; stty -a"), environment(runtime), sender(path, numbers))
            check(b"status=143" in written, name, written)
            check(modes_back(written), name, written)
            check(took < 6, name, took)

        # Ctrl+C with SIGINT ignored, as in a background job of a script: the run goes on.
        code, written, _ = play(sh(f"trap '' INT; {shlex.quote(aglow)} run far there x; echo status=$?; stty -a"),
                                environment(runtime), lambda _: [b"\x03", path_reply + GUARD])
        check(b"status=3" in written and b"there x pane=0" in written, written)
        check(modes_back(written), written)

        # Ctrl+C: aglow reads up to the guard, so that the reply does not reach the shell, and then ends by
        # SIGINT. dd reads what the shell would get as input.
        code, written, _ = play(sh(f"trap 'echo trapped' INT; {shlex.quote(aglow)} run far there; echo status=$?;"
                                   " stty -a; stty -icanon min 0 time 5; echo left:;"
                                   " dd bs=256 count=1 2>/dev/null | od -An -tx1; echo end"),
                                environment(runtime), lambda _: [b"\x03", path_reply + GUARD])
        check(b"status=130" in written and b"interrupted while it asked the terminal" in written, written)
        left = written[written.rfind(b"left:"):written.rfind(b"end")]
        check(b"1b" not in left, "a late reply reached the shell", left)
        check(modes_back(written), written)
    finally:
        server.close()
    print("aglow run: signals and Ctrl+C give the terminal its modes back", flush=True)


def prune_cases(aglow, scratch):
    """aglow remote-prune removes the sockets that nothing holds, and nothing else."""
    directory = os.path.join(scratch, "prune")
    os.mkdir(directory, 0o700)
    dead = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    dead.bind(os.path.join(directory, "dead.sock"))
    dead.close()
    live = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    live.bind(os.path.join(directory, "live.sock"))
    live.listen(1)
    with open(os.path.join(directory, "file.sock"), "w") as f:
        f.write("x")
    try:
        result = subprocess.run([aglow, "remote-prune", directory], stdin=subprocess.DEVNULL, capture_output=True,
                                start_new_session=True, timeout=20)
        check(result.returncode == 0, result)
        check(sorted(os.listdir(directory)) == ["file.sock", "live.sock"], os.listdir(directory))
    finally:
        live.close()
    print("aglow remote-prune: passed", flush=True)


def main():
    aglow = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="agr-", dir="/tmp") as scratch:
        direct_cases(aglow, scratch)
        print("aglow run: the terminal query works", flush=True)
        unasked_cases(aglow, scratch)
        signal_cases(aglow, scratch)
        prune_cases(aglow, scratch)


if __name__ == "__main__":
    main()
