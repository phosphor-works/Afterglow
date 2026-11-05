#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise OSC 5113 between a client and a terminal, with the same checks in each mode.

  --relay R                  kitten transfer against Afterglow's service (R is afterglow_transfer_tests)
  --relay R --aglow A        aglow transfer against Afterglow's service
  --kitty --aglow A          aglow transfer against kitty, in a headless nested KWin with a private bus.
                             Exits with 77 (skipped) when kitty or kwin_wayland is not installed.
"""
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
import shutil
import signal
import sys
import subprocess
import tempfile
import time


def check(condition, *detail):
    """Raises AssertionError when condition is false. Not assert: python -O must
    not turn the checks off."""
    if not condition:
        raise AssertionError(*detail)


def transfer(relay, arguments, client=("kitten", "transfer")):
    helper = subprocess.Popen([relay, "--protocol-relay"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    pid, master = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 1200, 800))
        os.environ["TERM"] = "xterm-kitty"
        os.execvp(client[0], [*client, *arguments])
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
                check(os.waitstatus_to_exitcode(status) == 0, transcript[-4000:].decode(errors="replace"))
                # The last packets (finish) can arrive after the last read. Read to EOF.
                while True:
                    try:
                        data = os.read(master, 65536)
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
                        data = b""
                    if not data:
                        break
                    transcript += data
                    buffer += data
                for match in re.finditer(rb"\x1b\]5113;(.*?)(?:\x1b\\|\x07)", buffer, re.S):
                    helper.stdin.write(base64.b64encode(match[1]) + b"\n")
                # EOF ends the relay after it handles every packet. Drain its replies meanwhile.
                helper.communicate(timeout=10)
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
        if helper.poll() is None:
            helper.kill()
            helper.wait(timeout=5)


# The password that both sides know, so that kitty asks no question.
BYPASS = "afterglow-interop"


class Kitty:
    """kitty as the terminal, in a headless KWin. Each transfer is one kitty
    window that runs aglow and closes when it ends."""

    def __init__(self, root):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from nested_desktop import KWINRC  # one KWin configuration for every runner

        self.root = root
        config = root / "config"
        config.mkdir()
        (config / "kwinrc").write_text(KWINRC)
        self.socket = f"afterglow-kitty-{os.getpid()}"
        self.env = dict(os.environ, XDG_CONFIG_HOME=str(config), WAYLAND_DISPLAY="", QT_QPA_PLATFORM="offscreen")
        for name in ("AFTERGLOW_SOCKET", "AFTERGLOW_PANE_ID", "KITTY_WINDOW_ID", "KITTY_PUBLIC_KEY", "DISPLAY"):
            self.env.pop(name, None)
        self.log = (root / "kwin.log").open("w")
        self.kwin = subprocess.Popen(
            ["kwin_wayland", "--virtual", "--socket", self.socket, "--width", "1200", "--height", "800",
             "--no-lockscreen", "--no-global-shortcuts", "--no-kactivities"],
            env=self.env, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while not (Path(self.env["XDG_RUNTIME_DIR"]) / self.socket).exists():
            check(self.kwin.poll() is None, "KWin exited at startup")
            check(time.monotonic() < deadline, "KWin did not create its socket in 10 s")
            time.sleep(0.05)
        self.env.update(WAYLAND_DISPLAY=self.socket)

    def close(self):
        self.kwin.terminate()
        try:
            self.kwin.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.kwin.kill()
            self.kwin.wait()
        self.log.close()

    def transfer(self, aglow, arguments):
        status = self.root / "status"
        errors = self.root / "errors"
        status.unlink(missing_ok=True)
        # kitty exits when its only window closes, so a shell keeps the exit status of aglow.
        command = ["kitty", "--config", "NONE", "-o", "file_transfer_confirmation_bypass=" + BYPASS,
                   "-o", "confirm_os_window_close=0", "-o", "linux_display_server=wayland",
                   "sh", "-c", 'status=$1 errors=$2; shift 2; "$@" 2>"$errors"; echo $? >"$status"',
                   "sh", str(status), str(errors), aglow, "transfer", "--permissions-bypass=" + BYPASS, *arguments]
        with (self.root / "kitty.log").open("a") as log:
            terminal = subprocess.Popen(command, env=self.env, stdout=log, stderr=subprocess.STDOUT, cwd=self.root)
            try:
                terminal.wait(timeout=60)
            except subprocess.TimeoutExpired:
                terminal.kill()
                terminal.wait()
                raise AssertionError("kitty did not finish the transfer in 60 s:\n"
                                     + (self.root / "kitty.log").read_text()[-3000:])
        check(status.exists(), "kitty ran no transfer:\n" + (self.root / "kitty.log").read_text()[-3000:])
        check(status.read_text().strip() == "0", "aglow transfer failed:\n" + errors.read_text()[-3000:])


def run_checks(run, client_bugs=False, allow_empty_bug=False):
    """Runs every transfer of the suite through run(arguments)."""
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
        # Directory metadata is applied by finish, after the children exist.
        modified = 1_600_000_000
        os.chmod(source / "nested", 0o750)
        os.utime(source / "nested", (modified, modified))
        for direction in ("send", "receive"):
            destination = root / direction
            destination.mkdir()
            run(["--direction=" + direction, str(source), str(destination) + "/"])
            copied = destination / "source"
            check((copied / "file").read_bytes() == contents)
            check((copied / "symlink").is_symlink())
            check((copied / "symlink").read_bytes() == contents)
            check((copied / "file").stat().st_ino == (copied / "hardlink").stat().st_ino)
            # kitten applies no directory metadata when it receives. aglow does.
            if direction == "send" or not client_bugs:
                nested = (copied / "nested").stat()
                check(nested.st_mode & 0o777 == 0o750, oct(nested.st_mode))
                check(int(nested.st_mtime) == modified, nested.st_mtime)
            empty = copied / "nested" / "empty"
            if direction == "receive" and not empty.exists() and allow_empty_bug:
                print("Client omitted an empty received file (known kitten 0.49.2 bug).", flush=True)
            else:
                check(empty.read_bytes() == b"")
            print(direction + ": recursive compressed transfer and links passed", flush=True)
        for direction in ("send", "receive"):
            destination = root / (direction + "-delta")
            destination.write_bytes(contents)
            changed = root / "changed"
            changed.write_bytes(b"inserted prefix\n" + contents + b"suffix\n")
            run(["--direction=" + direction, "--transmit-deltas", str(changed), str(destination)])
            check(destination.read_bytes() == changed.read_bytes())
            print(direction + ": delta transfer passed", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--relay", help="afterglow_transfer_tests, the service as the terminal")
    parser.add_argument("--aglow", help="aglow as the client in place of kitten")
    parser.add_argument("--kitty", action="store_true", help="kitty as the terminal (needs --aglow)")
    parser.add_argument("--allow-client-empty-file-bug", action="store_true")
    parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.kitty:
        if not args.aglow:
            parser.error("--kitty needs --aglow")
        missing = [tool for tool in ("kitty", "kwin_wayland", "dbus-run-session") if not shutil.which(tool)]
        if missing:
            print("Skipped: " + ", ".join(missing) + " not installed.", flush=True)
            sys.exit(77)
        aglow = str(Path(args.aglow).resolve())
        if not args.private_bus:
            # KWin and kitty must not join the user's session bus.
            sys.exit(subprocess.call(["dbus-run-session", "--", sys.executable, __file__, *sys.argv[1:],
                                      "--private-bus"]))
        with tempfile.TemporaryDirectory(prefix="afterglow-kitty-terminal-") as temporary:
            kitty = Kitty(Path(temporary))
            try:
                run_checks(lambda arguments: kitty.transfer(aglow, arguments))
            finally:
                kitty.close()
        return
    if not args.relay:
        parser.error("--relay is required without --kitty")
    relay = str(Path(args.relay).resolve())
    if args.aglow:
        client = (str(Path(args.aglow).resolve()), "transfer")
        run_checks(lambda arguments: transfer(relay, arguments, client))
    else:
        run_checks(lambda arguments: transfer(relay, arguments), client_bugs=True,
                   allow_empty_bug=args.allow_client_empty_file_bug)


if __name__ == "__main__":
    main()
