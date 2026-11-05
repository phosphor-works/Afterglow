#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""aglow clipboard at its edges, against the terminal that clipboard_pty.py plays.

Usage: clipboard_cases.py AGLOW

It checks that an output link to a file that does not exist yet creates the
file, that arguments a write cannot take are refused before the terminal sees
them (two files of one MIME type, standard output as an input, standard input
twice, a MIME type with a space), and that an OSC 52 read which the terminal
never answers ends after the timeout.
"""
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clipboard_pty import check, run  # noqa: E402


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

        # A terminal without OSC 5522 that answers DA1 but never the OSC 52 read.
        start = time.monotonic()
        code = run(aglow, ["--get-clipboard", "silent.txt"], root, clipboard, modern=False, osc52_reply=b"", timeout=15)
        check(code == 1 and time.monotonic() - start < 10, code)
        check(not (root / "silent.txt").exists())
        print("an OSC 52 read without an answer: passed", flush=True)


if __name__ == "__main__":
    main()
