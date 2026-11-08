#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise OSC 5113 between a client and a terminal, with the same checks in each mode.

  --relay R                  kitten transfer against Afterglow's service (R is afterglow_transfer_tests).
                             Exits with 77 (skipped) when kitten is not installed.
  --relay R --aglow A        aglow transfer against Afterglow's service
  --kitty --aglow A          aglow transfer against Kitty, in a headless nested KWin with a private bus.
                             Exits with 77 (skipped) when kitty, kwin_wayland or dbus-run-session is not
                             installed, or XDG_RUNTIME_DIR is not set.

The relay modes check in what the client writes that the recursive transfers use zlib and the delta transfers
rsync. In --kitty mode the bytes go between Kitty and aglow only, so that check is left out. Kitty, KWin and the
private bus get a home and XDG directories of their own, so that they never read or write the user's.
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nested_desktop import KWINRC, exit_on_sigterm, run_on_private_bus  # noqa: E402

PACKET = re.compile(rb"\x1b\]5113;(.*?)(?:\x1b\\|\x07)", re.S)


def check(condition, *detail):
    """Raises AssertionError when condition is false. Not assert: python -O must
    not turn the checks off."""
    if not condition:
        raise AssertionError(*detail)


def read_pty(fd):
    """What the pseudo-terminal FD holds, b"" once its other side has closed, None when nothing is there yet."""
    try:
        return os.read(fd, 65536)
    except BlockingIOError:
        return None
    except OSError as error:
        if error.errno == errno.EIO:
            return b""
        raise


def transfer(relay, arguments, client=("kitten", "transfer"), confirm=None, status=0):
    """Runs CLIENT ARGUMENTS in a pseudo-terminal whose OSC 5113 packets go to the service in RELAY, and its
    replies back. Returns what the client wrote. Both directions are queued, and written only when the other side
    can take them: a blocking write could wait for a client that waits for this script to read. CONFIRM is typed
    once the client asks to confirm its paths (--confirm-paths). The client must exit with STATUS."""
    helper = subprocess.Popen([relay, "--protocol-relay"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    pid, master = pty.fork()
    if pid == 0:
        # The child must never return into this script, even when exec fails.
        try:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 1200, 800))
            os.environ["TERM"] = "xterm-kitty"
            os.execvp(client[0], [*client, *arguments])
        finally:
            os._exit(127)
    os.set_blocking(master, False)
    os.set_blocking(helper.stdin.fileno(), False)
    buffer = b""
    transcript = b""
    to_client = bytearray()
    to_relay = bytearray()
    client_open = True
    reaped = False
    confirmed = False

    def take_packets():
        nonlocal buffer
        end = 0
        for match in PACKET.finditer(buffer):
            to_relay.extend(base64.b64encode(match[1]) + b"\n")
            end = match.end()
        buffer = buffer[end:]

    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            readers = [helper.stdout] + ([master] if client_open else [])
            writers = ([master] if to_client and client_open else []) + ([helper.stdin] if to_relay else [])
            readable, writable, _ = select.select(readers, writers, [], 0.05)
            if master in readable:
                data = read_pty(master)
                if data == b"":
                    client_open = False
                elif data:
                    transcript += data
                    buffer += data
                    take_packets()
                    if confirm is not None and not confirmed and b"to continue or" in transcript:
                        to_client.extend(confirm)
                        confirmed = True
            if helper.stdout in readable:
                data = os.read(helper.stdout.fileno(), 65536)
                if not data:
                    raise AssertionError(f"The relay exited with {helper.wait()} before the client ended:\n"
                                         + transcript[-4000:].decode(errors="replace"))
                to_client.extend(data)
            for target, fd, queue in ((master, master, to_client), (helper.stdin, helper.stdin.fileno(), to_relay)):
                if target in writable:
                    try:
                        del queue[:os.write(fd, queue)]
                    except BlockingIOError:
                        pass
            found, exited = os.waitpid(pid, os.WNOHANG)
            if found:
                reaped = True
                check(os.waitstatus_to_exitcode(exited) == status, os.waitstatus_to_exitcode(exited),
                      transcript[-4000:].decode(errors="replace"))
                # The last packets (finish) can arrive after the last read. Read to EOF.
                while client_open:
                    data = read_pty(master)
                    if not data:
                        break
                    transcript += data
                    buffer += data
                take_packets()
                # EOF ends the relay after it handles every packet. Drain its replies meanwhile.
                os.set_blocking(helper.stdin.fileno(), True)
                helper.communicate(input=bytes(to_relay), timeout=10)
                check(helper.returncode == 0, f"the relay exited with {helper.returncode}")
                return transcript
        raise AssertionError("The client timed out:\n" + transcript[-6000:].decode(errors="replace"))
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
        self.root = root
        config = root / "config"
        config.mkdir()
        (config / "kwinrc").write_text(KWINRC)
        self.socket = f"afterglow-kitty-{os.getpid()}"
        self.env = dict(os.environ, XDG_CONFIG_HOME=str(config), WAYLAND_DISPLAY="", QT_QPA_PLATFORM="offscreen")
        for name in ("AFTERGLOW_SOCKET", "AFTERGLOW_PANE_ID", "KITTY_WINDOW_ID", "KITTY_PUBLIC_KEY", "DISPLAY"):
            self.env.pop(name, None)
        self.log = None
        self.kwin = None

    def start(self):
        self.log = (self.root / "kwin.log").open("w")
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
        if self.kwin:
            self.kwin.terminate()
            try:
                self.kwin.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.kwin.kill()
                self.kwin.wait()
        if self.log:
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
            # A group of its own, so that a stall ends kitty, its shell and aglow together.
            terminal = subprocess.Popen(command, env=self.env, stdout=log, stderr=subprocess.STDOUT, cwd=self.root,
                                        start_new_session=True)
            try:
                terminal.wait(timeout=60)
            except subprocess.TimeoutExpired:
                os.killpg(terminal.pid, signal.SIGKILL)
                terminal.wait()
                raise AssertionError("kitty did not finish the transfer in 60 s:\n"
                                     + (self.root / "kitty.log").read_text()[-3000:])
        check(status.exists(), "kitty ran no transfer:\n" + (self.root / "kitty.log").read_text()[-3000:])
        check(status.read_text().strip() == "0", "aglow transfer failed:\n" + errors.read_text()[-3000:])
        return None


def run_checks(run, client_bugs=False, allow_empty_bug=False):
    """Runs every transfer of the suite through run(arguments), which returns what the client wrote, or None when
    the script cannot see it."""
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
            written = run(["--direction=" + direction, str(source), str(destination) + "/"])
            check(written is None or b"zip=zlib" in written, "the recursive transfer is not compressed")
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
            written = run(["--direction=" + direction, "--transmit-deltas", str(changed), str(destination)])
            check(written is None or b"tt=rsync" in written, "the delta transfer sends no rsync request")
            check(destination.read_bytes() == changed.read_bytes())
            print(direction + ": delta transfer passed", flush=True)


def confirm_checks(relay, client):
    """aglow transfer --confirm-paths: the list of transfers, then y goes on and n stops before any file moves."""
    with tempfile.TemporaryDirectory(prefix="afterglow-confirm-") as temporary:
        root = Path(temporary)
        source = root / "source.txt"
        source.write_bytes(b"confirmed\n")
        for direction in ("send", "receive"):
            for answer, status in ((b"y", 0), (b"n", 1)):
                destination = root / f"{direction}-{answer.decode()}"
                destination.mkdir()
                written = transfer(relay, ["--direction=" + direction, "--confirm-paths", str(source), str(destination) + "/"],
                                   client, confirm=answer, status=status)
                check(b"The following file transfers will be performed" in written, written[-2000:])
                # The list names where the file goes, on the side that gets it.
                check(f"{source} \u2192 {destination}/source.txt".encode() in written, written[-2000:])
                copied = destination / "source.txt"
                if answer == b"y":
                    check(copied.read_bytes() == b"confirmed\n")
                else:
                    check(not copied.exists() and not any(destination.iterdir()), list(destination.iterdir()))
            print(direction + ": --confirm-paths passed", flush=True)


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
        if not os.environ.get("XDG_RUNTIME_DIR"):
            print("Skipped: XDG_RUNTIME_DIR is not set, and KWin makes its socket there.", flush=True)
            sys.exit(77)
        aglow = str(Path(args.aglow).resolve())
        if not args.private_bus:
            # KWin, kitty and the private bus must not join the user's session bus, reach the user's compositor
            # or use the user's home: a service that the bus starts inherits this environment.
            with tempfile.TemporaryDirectory(prefix="afterglow-kitty-home-") as home:
                for name, sub in (("HOME", ""), ("XDG_CONFIG_HOME", ".config"), ("XDG_CACHE_HOME", ".cache"),
                                  ("XDG_DATA_HOME", ".local/share"), ("XDG_STATE_HOME", ".local/state")):
                    (Path(home) / sub).mkdir(parents=True, exist_ok=True)
                    os.environ[name] = str(Path(home) / sub)
                for name in ("WAYLAND_DISPLAY", "DISPLAY"):
                    os.environ.pop(name, None)
                # Four transfers of at most 60 s each, and the start of KWin. -I as CTest runs this
                # script: the run on the private bus reads no PYTHON* variable and no user site either.
                sys.exit(run_on_private_bus([sys.executable, "-I", __file__, *sys.argv[1:], "--private-bus"], 600))
        exit_on_sigterm()
        with tempfile.TemporaryDirectory(prefix="afterglow-kitty-terminal-") as temporary:
            kitty = Kitty(Path(temporary))
            try:
                kitty.start()
                run_checks(lambda arguments: kitty.transfer(aglow, arguments))
            finally:
                kitty.close()
        return
    if not args.relay:
        parser.error("--relay is required without --kitty")
    if not args.aglow and not shutil.which("kitten"):
        print("Skipped: kitten is not installed.", flush=True)
        sys.exit(77)
    relay = str(Path(args.relay).resolve())
    if args.aglow:
        client = (str(Path(args.aglow).resolve()), "transfer")
        run_checks(lambda arguments: transfer(relay, arguments, client))
        confirm_checks(relay, client)
    else:
        run_checks(lambda arguments: transfer(relay, arguments), client_bugs=True,
                   allow_empty_bug=args.allow_client_empty_file_bug)


if __name__ == "__main__":
    main()
