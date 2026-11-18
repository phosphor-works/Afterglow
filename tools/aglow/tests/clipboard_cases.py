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
the list of types comes from the reply for "." only, that a PNG asked for is
made of another image on the clipboard (the lossless one first, with its
alpha, the first frame of an animation, a message and no file for an image
that does not decode), and that Ctrl+C ends a write to a FIFO that has no
reader. aglow runs in a pseudo-terminal that the test makes, or in a session
of its own with no controlling terminal: it never reaches the terminal that
runs the test.
"""
import os
import re
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import clipboard_pty  # noqa: E402
from clipboard_pty import b64, check, run  # noqa: E402
from icat_decode import JPEG_ADOBE, WEBP_ANIMATED, bmp, gif, near  # noqa: E402


# The numbers of open and openat, for a kernel that shows no wait channel.
OPEN_SYSCALLS = {"x86_64": {"2", "257"}, "aarch64": {"56"}}.get(os.uname().machine, set())


def wait_channel(pid):
    """The wait channel of PID, or None when the kernel does not show it."""
    try:
        wchan = Path(f"/proc/{pid}/wchan").read_text().strip()
    except OSError:
        return None
    return None if wchan in ("", "0") else wchan


def open_call(pid):
    """The line of /proc/PID/syscall when PID is in open() or openat(), or None."""
    try:
        line = Path(f"/proc/{pid}/syscall").read_text().strip()
    except OSError:
        return None
    return line if line.split()[:1] and line.split()[0] in OPEN_SYSCALLS else None


def can_see_open(directory):
    """Whether blocked_in_open() sees a child that waits in open() of a FIFO in DIRECTORY. The child is a probe:
    this process cannot read a wait channel of its own, because it runs while it reads it."""
    fifo = Path(directory) / "probe-fifo"
    os.mkfifo(fifo)
    pid = os.fork()
    if pid == 0:
        # The child must never return into this script, which would remove the directory of the test.
        try:
            os.open(fifo, os.O_WRONLY)
        finally:
            os._exit(0)
    try:
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if blocked_in_open(pid):
                return True
            time.sleep(0.01)
        return False
    finally:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        fifo.unlink()


def blocked_in_open(pid):
    """Whether PID waits in open() for the other end of a FIFO. Without a wait channel, the same open() call
    must show two times, 50 ms apart: an open() that does not block is over by then."""
    wchan = wait_channel(pid)
    if wchan is not None:
        return wchan in ("wait_for_partner", "fifo_open")
    first = open_call(pid)
    if first is None:
        return False
    time.sleep(0.05)
    return open_call(pid) == first


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


def png_pixels(data):
    """The (r, g, b, a) pixels of a PNG of 8-bit RGBA whose rows have filter 0, as aglow writes it."""
    check(data.startswith(b"\x89PNG\r\n\x1a\n"), data[:8])
    at, chunks = 8, {}
    while at < len(data):
        length = struct.unpack(">I", data[at:at + 4])[0]
        kind = data[at + 4:at + 8]
        check(zlib.crc32(data[at + 4:at + 8 + length]) == struct.unpack(">I", data[at + 8 + length:at + 12 + length])[0], kind)
        chunks[kind] = chunks.get(kind, b"") + data[at + 8:at + 8 + length]
        at += 12 + length
    width, height, depth, colour = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
    check(depth == 8 and colour == 6, depth, colour)
    raw = zlib.decompress(chunks[b"IDAT"])
    row = 1 + width * 4
    out = []
    for y in range(height):
        check(raw[y * row] == 0)
        line = raw[y * row + 1:(y + 1) * row]
        out += [tuple(line[x:x + 4]) for x in range(0, len(line), 4)]
    return width, height, out


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: clipboard_cases.py AGLOW")
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
                                timeout=30, start_new_session=True)
        err = result.stderr.decode(errors="replace")
        check(result.returncode == 1 and "with the inputs before it" in err and "128.0 MiB" in err, result.returncode, err)
        print("inputs too large together: passed", flush=True)

        # A PNG asked for and none on the clipboard: aglow makes one of another image there, the
        # lossless one first, and a JPEG's profile converts to sRGB.
        picture = bmp(2, [[(10, 20, 30), (200, 100, 50)]])
        images = {"image/jpeg": JPEG_ADOBE, "image/bmp": picture}
        check(run(aglow, ["--get-clipboard", "from-bmp.png"], root, images) == 0)
        check(png_pixels((root / "from-bmp.png").read_bytes()) == (2, 1, [(10, 20, 30, 255), (200, 100, 50, 255)]))
        check(run(aglow, ["--get-clipboard", "from-jpeg.png"], root, {"image/jpeg": JPEG_ADOBE}) == 0)
        width, height, got = png_pixels((root / "from-jpeg.png").read_bytes())
        check((width, height) == (8, 8) and all(near(px, (66, 151, 202, 255)) for px in got), width, height, got[:2])
        # The alpha of the image: a GIF whose second pixel is transparent.
        transparent = gif(2, 1, 2, [{"rect": (0, 0, 1, 1), "indices": [0]}])
        check(run(aglow, ["--get-clipboard", "from-gif.png"], root, {"image/gif": transparent}) == 0)
        check(png_pixels((root / "from-gif.png").read_bytes()) == (2, 1, [(255, 0, 0, 255), (0, 0, 0, 0)]))
        # The first frame of an animation: a WebP, red then blue. With a GIF there too, the GIF, which is
        # lossless, goes first.
        check(run(aglow, ["--get-clipboard", "from-webp.png"], root, {"image/webp": WEBP_ANIMATED}) == 0)
        check(png_pixels((root / "from-webp.png").read_bytes()) == (4, 4, [(255, 0, 0, 255)] * 16))
        check(run(aglow, ["--get-clipboard", "lossless.png"], root, {"image/webp": WEBP_ANIMATED, "image/gif": transparent}) == 0)
        check(png_pixels((root / "lossless.png").read_bytes())[:2] == (2, 1))
        # The type itself when it is there, and an image that does not decode: an error that says why, and no file.
        check(run(aglow, ["--get-clipboard", "as-is.jpg"], root, images) == 0)
        check((root / "as-is.jpg").read_bytes() == JPEG_ADOBE)
        said = bytearray()
        check(run(aglow, ["--get-clipboard", "broken.png"], root, {"image/jpeg": b"\xff\xd8\xff not a jpeg"},
                  transcript=said) == 1)
        check(b"Could not make a PNG of the image/jpeg on the clipboard for broken.png: " in said, bytes(said))
        check(not (root / "broken.png").exists())
        check(run(aglow, ["--get-clipboard", "none.png"], root, clipboard) == 1)
        check(not (root / "none.png").exists())
        print("a PNG made of another image: passed", flush=True)

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
        # it. The terminal has been restored when aglow opens its output, so the signal comes only once
        # aglow waits in that open().
        os.mkfifo(root / "fifo")
        fork = clipboard_pty.pty.fork
        if not can_see_open(root):
            print("a FIFO without a reader: skipped, this kernel does not show a process that waits in open()",
                  flush=True)
            sys.exit(77)
        stop = threading.Event()
        blocked = threading.Event()
        watchers = []

        def interrupt_when_blocked(pid):
            deadline = time.monotonic() + 8
            while not stop.is_set() and time.monotonic() < deadline:
                if blocked_in_open(pid):
                    blocked.set()
                    try:
                        os.kill(pid, signal.SIGINT)
                    except ProcessLookupError:
                        pass
                    return
                stop.wait(0.01)

        def interrupt_later():
            pid, fd = fork()
            if pid:
                watcher = threading.Thread(target=interrupt_when_blocked, args=(pid,), daemon=True)
                watchers.append(watcher)
                watcher.start()
            return pid, fd

        clipboard_pty.pty.fork = interrupt_later
        try:
            code = run(aglow, ["--get-clipboard", "--mime", "text/plain", "fifo"], root, clipboard, timeout=10)
        finally:
            clipboard_pty.pty.fork = fork
            stop.set()
            for watcher in watchers:
                watcher.join()
        check(blocked.is_set(), "aglow never waited in open() of the FIFO")
        check(code == -signal.SIGINT, code)
        print("a FIFO without a reader: passed", flush=True)


if __name__ == "__main__":
    main()
