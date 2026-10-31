#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise OSC 5113 with the installed kitten client and an isolated service."""
import argparse
import base64
import errno
import fcntl
import struct
import termios
import os
from pathlib import Path
import pty
import re
import select
import signal
import subprocess
import tempfile
import time


def transfer(relay, arguments):
    helper = subprocess.Popen([relay, "--protocol-relay"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    pid, master = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 1200, 800))
        os.environ["TERM"] = "xterm-kitty"
        os.execvp("kitten", ["kitten", "transfer", *arguments])
    buffer = b""
    transcript = b""
    status = None
    reaped = False
    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master, helper.stdout], [], [], 0.05)
            for source in ready:
                try:
                    data = os.read(source if source == master else source.fileno(), 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        data = b""
                    else:
                        raise
                if source != master:
                    if data:
                        os.write(master, data)
                    continue
                transcript += data
                buffer += data
                for match in re.finditer(rb"\x1b\]5113;(.*?)(?:\x1b\\|\x07)", buffer, re.S):
                    helper.stdin.write(base64.b64encode(match[1]) + b"\n")
                    helper.stdin.flush()
                complete = list(re.finditer(rb"\x1b\]5113;.*?(?:\x1b\\|\x07)", buffer, re.S))
                if complete:
                    buffer = buffer[complete[-1].end():]
            found, status = os.waitpid(pid, os.WNOHANG)
            if found:
                reaped = True
                assert os.waitstatus_to_exitcode(status) == 0, transcript[-4000:].decode(errors="replace")
                # finish is asynchronous; let the relay commit before EOF.
                time.sleep(0.1)
                return
        raise AssertionError("Kitten transfer timed out:\n" + transcript[-6000:].decode(errors="replace"))
    finally:
        if not reaped:
            try:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            except ProcessLookupError:
                pass
        os.close(master)
        helper.stdin.close()
        helper.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--relay", required=True)
    parser.add_argument("--allow-client-empty-file-bug", action="store_true")
    args = parser.parse_args()
    relay = str(Path(args.relay).resolve())
    with tempfile.TemporaryDirectory(prefix="afterglow-kitty-") as temporary:
        root = Path(temporary)
        source = root / "source"
        source.mkdir()
        contents = (b"protocol interoperability\n" * 20000) + os.urandom(60000)
        (source / "file").write_bytes(contents)
        (source / "symlink").symlink_to("file")
        os.link(source / "file", source / "hardlink")
        (source / "nested").mkdir()
        (source / "nested" / "empty").touch()
        for direction in ("send", "receive"):
            destination = root / direction
            destination.mkdir()
            transfer(relay, ["--direction=" + direction, str(source), str(destination) + "/"])
            copied = destination / "source"
            assert (copied / "file").read_bytes() == contents
            assert (copied / "symlink").is_symlink()
            assert (copied / "symlink").read_bytes() == contents
            assert (copied / "file").stat().st_ino == (copied / "hardlink").stat().st_ino
            empty = copied / "nested" / "empty"
            if direction == "receive" and not empty.exists() and args.allow_client_empty_file_bug:
                print("Client omitted an empty received file (known kitten 0.49.2 bug).", flush=True)
            else:
                assert empty.read_bytes() == b""
            print(direction + ": recursive compressed transfer and links passed", flush=True)
        for direction in ("send", "receive"):
            destination = root / (direction + "-delta")
            destination.write_bytes(contents)
            changed = root / "changed"
            changed.write_bytes(b"inserted prefix\n" + contents + b"suffix\n")
            transfer(relay, ["--direction=" + direction, "--transmit-deltas", str(changed), str(destination)])
            assert destination.read_bytes() == changed.read_bytes()
            print(direction + ": delta transfer passed", flush=True)


if __name__ == "__main__":
    main()
