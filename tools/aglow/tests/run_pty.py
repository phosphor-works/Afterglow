#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""aglow run on a remote host: the query for the socket of the ssh session (OSC 24569).

Usage: run_pty.py AGLOW

This script plays the terminal in a pseudo-terminal that it makes, and a
terminal socket (the "remote" target) on a Unix socket of its own. It checks
the DA1 guard with a terminal that is not Afterglow, a path that is not on
this host, a reason in place of a path, and a run through the socket.
tmux_pty.py checks the query inside tmux. aglow never sees an
AFTERGLOW_SOCKET or a TMUX of the session that runs the test.
"""
import json
import os
import pty
import select
import socket
import sys
import tempfile
import threading
import time

QUERY = b"\x1b]24569;q=remote\x1b\\"


def check(condition, *detail):
    """Raises AssertionError when condition is false. Not assert: python -O must not turn the checks off."""
    if not condition:
        raise AssertionError(*detail)


class FakeTerminalSocket:
    """A phosphor-ipc server with the target "remote", as Afterglow forwards it."""

    def __init__(self, path):
        self.path = path
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
        if call.get("target") != "remote":
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


def play(argv, env, answer, timeout=20):
    """Runs ARGV in a pseudo-terminal. ANSWER(written) gives the bytes that the terminal sends for what the
    program wrote so far, once for each query. Returns (exit code, everything written, seconds taken)."""
    started = time.monotonic()
    pid, master = pty.fork()
    if pid == 0:
        try:
            os.execve(argv[0], argv, env)
        finally:
            os._exit(127)
    written = b""
    answered = 0
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
                if reply:
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


def direct_cases(aglow, scratch):
    runtime = os.path.join(scratch, "run")
    os.mkdir(runtime, 0o700)
    server = FakeTerminalSocket(os.path.join(scratch, "remote.sock"))
    argv = [aglow, "run", "far", "there", "a", "b"]
    try:
        # A terminal that is not Afterglow answers DA1 alone: no wait for the guard's time.
        code, written, took = play(argv, environment(runtime), lambda _: b"\x1b[?62;22c")
        check(written.startswith(QUERY + b"\x1b[c"), written)
        check(code == 1, code, written)
        check(b"no running Afterglow on this display" in written, written)
        check(took < 1.5, "aglow waited for the guard", took)

        # The path of a session on another host: a second ssh hop.
        code, written, _ = play(argv, environment(runtime),
                                lambda _: b"\x1b]24569;path=/nowhere/0123456789abcdef.sock\x1b\\\x1b[?62;22c")
        check(code == 1, code, written)
        check(b"/nowhere/0123456789abcdef.sock, is on another host" in written, written)

        # Each reason in place of a path.
        for reason, text in ((b"forward", b"AllowStreamLocalForwarding"), (b"version", b"another protocol"),
                             (b"length", b"over 104 bytes"), (b"off", b"Terminal.sshRemoteCommands")):
            code, written, _ = play(argv, environment(runtime),
                                    lambda _, r=reason: b"\x1b]24569;error=" + r + b"\x1b\\\x1b[?62;22c")
            check(code == 1, reason, code, written)
            check(text in written, reason, written)

        # The socket of the session: the "remote" target, on the session's pane.
        code, written, _ = play(argv, environment(runtime, {"AFTERGLOW_PANE_ID": "12"}),
                                lambda _: b"\x1b]24569;path=" + server.path.encode() + b"\x1b\\\x1b[?62;22c")
        check(code == 3, code, written)
        check(b"there a b pane=0\r\n" in written, written)
        check({call["target"] for call in server.calls} == {"remote"}, server.calls)

        # A pane's own socket on this host needs no query.
        server.calls.clear()
        code, written, _ = play(argv, environment(runtime, {"AFTERGLOW_SOCKET": server.path}), lambda _: None)
        check(QUERY not in written, written)
        # The local target is "commands", which the fake socket does not have.
        check(code == 1 and b"takes no commands" in written, code, written)
    finally:
        server.close()


def main():
    aglow = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="agr-", dir="/tmp") as scratch:
        direct_cases(aglow, scratch)
    print("aglow run: the terminal query works")


if __name__ == "__main__":
    main()
