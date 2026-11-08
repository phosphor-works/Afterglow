#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""aglow clipboard against a terminal that this script plays, in a pseudo-terminal that it makes.

Usage: clipboard_pty.py AGLOW

It checks that a read waits for a slow permission prompt, that output files
keep their mode or follow the umask, that a link is written through, that an
OSC 52 answer that is not valid is an error, and that a write reaches the
clipboard through OSC 5522 and through OSC 52.
"""
import base64
import os
import pty
import re
import select
import signal
import sys
import tempfile
import time
from pathlib import Path


def check(condition, *detail):
    """Raises AssertionError when condition is false. Not assert: python -O must not turn the checks off."""
    if not condition:
        raise AssertionError(*detail)


def b64(text):
    return base64.b64encode(text.encode()).decode()


def run(aglow, arguments, cwd, clipboard, modern=True, prompt=0.0, osc52_reply=None, umask=0o022, timeout=20,
        written=None):
    """Runs aglow clipboard ARGUMENTS against a terminal whose clipboard holds CLIPBOARD (MIME -> text).
    PROMPT seconds pass before the terminal answers a read, as a permission prompt takes. What aglow
    writes to the clipboard goes into the dict WRITTEN (MIME -> bytes, "OSC 52" for an OSC 52 write)."""
    pid, master = pty.fork()
    if pid == 0:
        # The child must never return into this script, even when exec fails.
        try:
            os.chdir(cwd)
            os.umask(umask)
            os.execv(aglow, [aglow, "clipboard", "--detection-timeout=0.5", *arguments])
        finally:
            os._exit(127)
    buffer = b""
    reaped = False
    pending = {}
    deadline = time.monotonic() + timeout

    def wait_exit():
        """The exit code of aglow once it has ended, None while it runs."""
        nonlocal reaped
        found, status = os.waitpid(pid, os.WNOHANG)
        if not found:
            return None
        reaped = True
        return os.waitstatus_to_exitcode(status)

    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.05)
            if ready:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    data = b""
                if not data:
                    # The program closed the terminal: it exits now, within the deadline.
                    while time.monotonic() < deadline:
                        code = wait_exit()
                        if code is not None:
                            return code
                        time.sleep(0.01)
                    break
                buffer += data
            while True:
                found = re.search(rb"\x1b\[\?5522\$p|\x1b\[c|\x1b\]5522;([^;\x1b]*);?([^\x1b]*)\x1b\\"
                                  rb"|\x1b\]52;([a-z0-9]*);([^\x1b]*)\x1b\\", buffer)
                if not found:
                    break
                buffer = buffer[found.end():]
                text = found.group(0)
                if text.startswith(b"\x1b[?5522"):
                    os.write(master, b"\x1b[?5522;%d$y" % (1 if modern else 0))
                elif text == b"\x1b[c":
                    os.write(master, b"\x1b[?62;22c")
                elif text.startswith(b"\x1b]52;"):
                    check(not modern, "aglow used OSC 52 with a terminal that has OSC 5522")
                    if found.group(4) != b"?":
                        if written is not None:
                            written["OSC 52"] = base64.b64decode(found.group(4))
                        continue
                    check(osc52_reply is not None, "the test gives no answer to an OSC 52 read")
                    time.sleep(prompt)
                    os.write(master, osc52_reply)
                else:
                    meta = dict(field.split("=", 1) for field in found.group(1).decode().split(":") if "=" in field)
                    kind = meta.get("type")
                    if kind == "write":
                        pending = {"id": meta.get("id", "")}
                        continue
                    if kind == "wdata":
                        if "mime" not in meta:
                            # The end of the write: the terminal takes it.
                            if written is not None:
                                written.update({mime: value for mime, value in pending.items() if mime != "id"})
                            os.write(master, f"\x1b]5522;type=write:status=DONE:id={pending.get('id', '')}\x1b\\".encode())
                            continue
                        mime = base64.b64decode(meta["mime"]).decode()
                        pending[mime] = pending.get(mime, b"") + base64.b64decode(found.group(2))
                        continue
                    if kind != "read":
                        continue
                    time.sleep(prompt)
                    ident = meta.get("id", "")
                    wanted = base64.b64decode(found.group(2)).decode().split()
                    out = f"\x1b]5522;type=read:status=OK:id={ident}\x1b\\"
                    for mime in wanted:
                        if mime == ".":
                            out += f"\x1b]5522;type=read:status=DATA:mime={b64('.')}:id={ident};{b64(' '.join(clipboard))}\x1b\\"
                        elif mime in clipboard:
                            out += f"\x1b]5522;type=read:status=DATA:mime={b64(mime)}:id={ident};{b64(clipboard[mime])}\x1b\\"
                    out += f"\x1b]5522;type=read:status=DONE:id={ident}\x1b\\"
                    os.write(master, out.encode())
            # No exit check here: aglow can write its last sequence and exit
            # between this select and the next. The end of the terminal's
            # output, after every byte has been read and parsed, gives the exit.
        raise AssertionError("aglow clipboard did not finish: " + repr(buffer[-500:]))
    finally:
        if not reaped:
            try:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            except ProcessLookupError:
                pass
        os.close(master)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: clipboard_pty.py AGLOW")
    aglow = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="aglow-clipboard-") as temporary:
        root = Path(temporary)
        clipboard = {"text/plain": "from the clipboard\n"}

        # The terminal answers two seconds after the request: longer than the
        # detection timeout, as a person who reads a permission prompt.
        code = run(aglow, ["--get-clipboard", "out.txt"], root, clipboard, prompt=2.0)
        check(code == 0, code)
        check((root / "out.txt").read_text() == "from the clipboard\n")
        check((root / "out.txt").stat().st_mode & 0o777 == 0o644, oct((root / "out.txt").stat().st_mode))
        print("a read waits for the permission prompt: passed", flush=True)

        # A new file follows the umask. An existing file keeps its mode.
        check(run(aglow, ["--get-clipboard", "private.txt"], root, clipboard, umask=0o077) == 0)
        check((root / "private.txt").stat().st_mode & 0o777 == 0o600)
        (root / "kept.txt").write_text("old")
        os.chmod(root / "kept.txt", 0o640)
        check(run(aglow, ["--get-clipboard", "kept.txt"], root, clipboard) == 0)
        check((root / "kept.txt").stat().st_mode & 0o777 == 0o640)
        check((root / "kept.txt").read_text() == "from the clipboard\n")
        print("output files follow the umask or keep their mode: passed", flush=True)

        # A link is written through, and stays a link.
        (root / "target.txt").write_text("old")
        (root / "link.txt").symlink_to("target.txt")
        check(run(aglow, ["--get-clipboard", "link.txt"], root, clipboard) == 0)
        check((root / "link.txt").is_symlink())
        check((root / "target.txt").read_text() == "from the clipboard\n")
        print("a link is written through: passed", flush=True)

        # OSC 52 in a terminal without OSC 5522: an answer that is not valid is an error.
        code = run(aglow, ["--get-clipboard", "bad.txt"], root, clipboard, modern=False, osc52_reply=b"\x1b]52;c\x1b\\")
        check(code == 1, code)
        check(not (root / "bad.txt").exists())
        code = run(aglow, ["--get-clipboard", "good.txt"], root, clipboard, modern=False,
                   osc52_reply=b"\x1b]52;c;" + base64.b64encode(b"legacy") + b"\x1b\\")
        check(code == 0, code)
        check((root / "good.txt").read_text() == "legacy")
        print("OSC 52 answers: passed", flush=True)

        # A write: the file and standard input reach the terminal's clipboard.
        (root / "page.html").write_text("<b>bold</b>")
        written = {}
        check(run(aglow, ["page.html"], root, clipboard, written=written) == 0)
        check(written.get("text/html") == b"<b>bold</b>", written)
        written = {}
        check(run(aglow, ["--mime", "text/plain", "out.txt"], root, clipboard, written=written) == 0)
        check(written.get("text/plain") == b"from the clipboard\n", written)
        written = {}
        check(run(aglow, ["out.txt"], root, clipboard, modern=False, written=written) == 0)
        check(written.get("OSC 52") == b"from the clipboard\n", written)
        print("writes through OSC 5522 and OSC 52: passed", flush=True)


if __name__ == "__main__":
    main()
