#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""aglow clipboard at its edges, against the terminal that clipboard_pty.py plays.

Usage: clipboard_cases.py AGLOW

It checks that an output link to a file that does not exist yet creates the
file, that arguments a write cannot take are refused before the terminal sees
them (two files of one MIME type, standard output as an input, standard input
twice, a MIME type with a space), that inputs too large together say so, that
an OSC 52 read which the terminal never answers ends after the timeout, that
the list of types comes from the reply for "." only, and that Ctrl+C ends a
write to a FIFO that has no reader.
"""
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import clipboard_pty  # noqa: E402
from clipboard_pty import b64, check, run  # noqa: E402


class ExtraReply:
    """os for clipboard_pty, with a DATA reply for text/html that the terminal adds to its list of types."""

    def __getattr__(self, name):
        return getattr(os, name)

    @staticmethod
    def write(fd, data):
        listing = re.search(rb"status=DATA:mime=" + b64(".").encode() + rb":id=([^;\x1b]*)", data)
        if listing:
            ident = listing.group(1).decode()
            extra = f"\x1b]5522;type=read:status=DATA:mime={b64('text/html')}:id={ident};{b64('image/png')}\x1b\\".encode()
            data = data.replace(b"\x1b]5522;type=read:status=DONE", extra + b"\x1b]5522;type=read:status=DONE", 1)
        return os.write(fd, data)


def main():
    aglow = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="aglow-clipboard-") as temporary:
        root = Path(temporary)
        clipboard = {"text/plain": "from the clipboard\n"}

        # A link whose target does not exist yet: the target is created, as a shell redirection does.
        (root / "dangling.txt").symlink_to("created.txt")
        check(run(aglow, ["--get-clipboard", "dangling.txt"], root, clipboard) == 0)
        check((root / "dangling.txt").is_symlink())
        check((root / "created.txt").read_text() == "from the clipboard\n")
        print("a link to a new file: passed", flush=True)

        # Writes that the terminal would refuse, or that would read the wrong stream.
        (root / "a.txt").write_text("a")
        (root / "b.txt").write_text("b")
        check(run(aglow, ["a.txt", "b.txt"], root, clipboard, timeout=10) == 1)
        check(run(aglow, ["/dev/stdout"], root, clipboard, timeout=10) == 1)
        check(run(aglow, ["--mime", "text/plain", "--mime", "text/html", "-", "/dev/stdin"], root, clipboard, timeout=10) == 1)
        check(run(aglow, ["--mime", "text/plain x", "a.txt"], root, clipboard, timeout=10) == 1)
        check(run(aglow, ["--alias", "text/plain=two words", "a.txt"], root, clipboard, timeout=10) == 1)
        print("arguments that a write cannot take: passed", flush=True)

        # Inputs that are too large together: the message names the limit, not what was left of it.
        limit = 128 * 1024 * 1024
        with open(root / "most.bin", "wb") as most:
            most.truncate(limit - 10)
        (root / "more.bin").write_bytes(b"x" * 100)
        result = subprocess.run([aglow, "clipboard", "--mime", "application/x-most", "--mime", "application/x-more",
                                 "most.bin", "more.bin"], cwd=root, stdin=subprocess.DEVNULL, capture_output=True,
                                timeout=30)
        err = result.stderr.decode(errors="replace")
        check(result.returncode == 1 and "with the inputs before it" in err and "128.0 MiB" in err, result.returncode, err)
        print("inputs too large together: passed", flush=True)

        # A terminal without OSC 5522 that answers DA1 but never the OSC 52 read.
        start = time.monotonic()
        code = run(aglow, ["--get-clipboard", "silent.txt"], root, clipboard, modern=False, osc52_reply=b"", timeout=15)
        check(code == 1 and time.monotonic() - start < 10, code)
        check(not (root / "silent.txt").exists())
        print("an OSC 52 read without an answer: passed", flush=True)

        # The list of types is the reply for "." only: data of another type is no list.
        clipboard_pty.os = ExtraReply()
        try:
            check(run(aglow, ["--get-clipboard", "--mime", ".", "types.txt"], root, clipboard) == 0)
        finally:
            clipboard_pty.os = os
        check((root / "types.txt").read_text() == "text/plain\n", (root / "types.txt").read_text())
        print("the list of types: passed", flush=True)

        # A FIFO without a reader: aglow waits in open(), as a shell redirection does, and Ctrl+C ends
        # it. The terminal has long been restored when the signal comes.
        os.mkfifo(root / "fifo")
        fork = clipboard_pty.pty.fork

        def interrupt(pid):
            try:
                os.kill(pid, signal.SIGINT)
            except ProcessLookupError:
                pass

        def interrupt_later():
            pid, fd = fork()
            if pid:
                threading.Timer(3, interrupt, (pid,)).start()
            return pid, fd

        clipboard_pty.pty.fork = interrupt_later
        try:
            code = run(aglow, ["--get-clipboard", "--mime", "text/plain", "fifo"], root, clipboard, timeout=10)
        finally:
            clipboard_pty.pty.fork = fork
        check(code == -signal.SIGINT, code)
        print("a FIFO without a reader: passed", flush=True)


if __name__ == "__main__":
    main()
