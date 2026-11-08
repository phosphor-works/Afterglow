#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""aglow inside tmux: the escape codes that must pass through tmux to the terminal.

Usage: tmux_pty.py AGLOW

Each case runs aglow in a private tmux server (tmux -L, a configuration of
its own and a socket directory of its own), in a pseudo-terminal that this
script makes and plays as the terminal. It never reaches a tmux of the
session that runs the test. With allow-passthrough on and off, it checks:

- aglow icat: the graphics commands reach the terminal. With the option off,
  aglow turns it on for its pane itself.
- aglow run: the query for the socket of an ssh session (OSC 24569) passes
  through, with the status report that ends the wait, and the reply comes
  back. The status report alone ends the wait too. With the option off,
  aglow names the option and does not wait.

Exit status 77: tmux is not installed, or older than 3.3, which has no
allow-passthrough.
"""
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

# The harness of run_pty.py, next to this script: python -I leaves its directory out of the path. The
# import writes no __pycache__ into the source tree.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_pty import QUERY, FakeTerminalSocket, check, environment, play  # noqa: E402

NAME = "aglow-tmux-test"
# aglow waits this long for the guard at most (Remote::GuardTimeoutMs). A run that the guard ends takes less.
GUARD_MS = 2000


def png(width, height, rgb):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\0" + bytes(rgb) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class Tmux:
    """A private tmux server, which the test ends."""

    def __init__(self, tmux, scratch):
        self.tmux = tmux
        self.scratch = scratch
        # tmux's socket path holds 108 bytes: a short directory of its own.
        self.sockets = tempfile.mkdtemp(prefix="agt-", dir="/tmp")
        self.env = environment(os.path.join(scratch, "run"), {"TMUX_TMPDIR": self.sockets})

    def run(self, passthrough, command, answer):
        """Runs COMMAND in a new session whose terminal is a pseudo-terminal, until it ends. Returns what tmux
        wrote to the terminal, and the seconds that it took."""
        config = os.path.join(self.scratch, f"tmux-{passthrough}.conf")
        with open(config, "w") as f:
            f.write(f"set -g allow-passthrough {passthrough}\nset -g escape-time 0\nset -g status off\n"
                    "set -g default-shell /bin/sh\n")
        code, written, took = play([self.tmux, "-L", NAME, "-f", config, "new-session", "-x", "80", "-y", "24", command],
                                   self.env, answer)
        self.kill()
        check(code == 0, "tmux failed", code, written[-400:])
        return written, took

    def kill(self):
        subprocess.run([self.tmux, "-L", NAME, "kill-server"], env=self.env, stderr=subprocess.DEVNULL, timeout=20)

    def close(self):
        self.kill()
        shutil.rmtree(self.sockets, ignore_errors=True)


def read(path):
    return open(path).read() if os.path.exists(path) else None


def icat_cases(aglow, scratch, tmux):
    image = os.path.join(scratch, "red.png")
    with open(image, "wb") as f:
        f.write(png(4, 2, (255, 0, 0)))
    for passthrough in ("on", "off"):
        out = os.path.join(scratch, f"icat-{passthrough}")
        q = shlex.quote
        command = (f"{q(aglow)} icat --stdin=no --use-window-size=80,24,800,480 {q(image)} > /dev/tty 2> {q(out + '.err')};"
                   f" echo $? > {q(out + '.status')}; tmux show -pv allow-passthrough > {q(out + '.option')}")
        written, _ = tmux.run(passthrough, command, lambda _: None)
        check(read(f"{out}.status") == "0\n", passthrough, read(f"{out}.err"))
        # tmux unwraps the passthrough: the terminal gets the graphics commands themselves. Inside tmux, aglow
        # places the image with Unicode placeholders, which tmux keeps in its own screen.
        check(b"\x1b_G" in written and b"U=1" in written, passthrough, written[-600:])
        check(b"\x1bPtmux;" not in written, written[-600:])
        # With the option off, aglow turned it on for its pane.
        check(read(f"{out}.option") == "on\n", passthrough, read(f"{out}.option"))
        print(f"icat in tmux with allow-passthrough {passthrough}: passed", flush=True)


def run_cases(aglow, scratch, tmux):
    os.mkdir(os.path.join(scratch, "run"), 0o700)
    server = FakeTerminalSocket(os.path.join(scratch, "tmux-remote.sock"))
    try:
        q = shlex.quote
        # (allow-passthrough, whether the terminal replies with the path or answers only the status report)
        for passthrough, replies in (("on", True), ("on", False), ("off", True)):
            out = os.path.join(scratch, f"run-{passthrough}-{replies}")
            # Standard output stays on the terminal: aglow asks only then. The time is aglow's own, without tmux.
            command = (f"s=$(date +%s%N); {q(aglow)} run far there t 2> {q(out + '.err')}; echo $? > {q(out + '.status')};"
                       f" e=$(date +%s%N); echo $(( (e - s) / 1000000 )) > {q(out + '.ms')}")

            def answer(written, replies=replies):
                # The query and the status report reach the terminal in one passthrough.
                reply = b"\x1b]24569;path=" + server.path.encode() + b"\x1b\\" if replies else b""
                return reply + b"\x1b[0n" if b"\x1b[5n" in written else reply

            written, _ = tmux.run(passthrough, command, answer)
            took = int(read(f"{out}.ms") or "999999")
            if passthrough == "on":
                check(QUERY in written, "the query did not pass through tmux", written[-400:])
                if replies:
                    check(read(f"{out}.status") == "3\n", read(f"{out}.status"), read(f"{out}.err"))
                    check(b"there t pane=0" in written, written[-400:], read(f"{out}.err"))
                else:
                    check(read(f"{out}.status") == "1\n", read(f"{out}.status"), read(f"{out}.err"))
                    check("no running Afterglow" in read(f"{out}.err"), read(f"{out}.err"))
                # The status report ended the wait, not the guard's time.
                check(took < GUARD_MS * 0.95, "aglow waited for the guard's time", took)
            else:
                check(QUERY not in written, written[-400:])
                check(read(f"{out}.status") == "1\n", read(f"{out}.err"))
                check("allow-passthrough" in read(f"{out}.err"), read(f"{out}.err"))
                # tmux said that the option is off: no query, and no wait for the guard.
                check(took < GUARD_MS * 0.95, took)
            print(f"run in tmux with allow-passthrough {passthrough} ({'reply' if replies else 'status report only'}):"
                  " passed", flush=True)
    finally:
        server.close()


def main():
    aglow = os.path.abspath(sys.argv[1])
    tmux_program = shutil.which("tmux")
    if not tmux_program:
        print("Skipped: tmux is not installed.", flush=True)
        sys.exit(77)
    # allow-passthrough came with tmux 3.3. A version that does not parse (next-3.4, master) is new.
    version = subprocess.run([tmux_program, "-V"], capture_output=True, text=True, timeout=20).stdout
    match = re.search(r"(\d+)\.(\d+)", version)
    if match and (int(match.group(1)), int(match.group(2))) < (3, 3):
        print(f"Skipped: {version.strip()} has no allow-passthrough (tmux 3.3 or later).", flush=True)
        sys.exit(77)
    with tempfile.TemporaryDirectory(prefix="agx-", dir="/tmp") as scratch:
        tmux = Tmux(tmux_program, scratch)
        try:
            icat_cases(aglow, scratch, tmux)
            run_cases(aglow, scratch, tmux)
        finally:
            tmux.close()


if __name__ == "__main__":
    main()
