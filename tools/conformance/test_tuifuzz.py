#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regression checks for afterglow_tuifuzz; pass its built executable.

Exits with 77 (skipped) when bwrap is not installed."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())


def check(condition, message):
    # Not assert: python -O would skip the checks.
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def run(args, env=None, timeout=60):
    return subprocess.run([binary, *args], capture_output=True, timeout=timeout, env=env)


# Bad options and values never run the command.
never = ["--", "sh", "-c", "exit 99"]
for args in (["--cols", "1"], ["--rows", "0"], ["--steps", "0"], ["--seed", "-1"], ["--steps", "x"],
             ["--env", "NOVALUE"], ["--env", "=x"], ["--bogus", "1"]):
    result = run([*args, *never])
    check(result.returncode == 2 and b"usage:" in result.stderr, (args, result.returncode, result.stderr))
for args in ([], ["--"], ["--steps"], ["sh"]):  # no command, or a command without "--"
    result = run(args)
    check(result.returncode == 2 and b"usage:" in result.stderr, (args, result.returncode, result.stderr))

if not shutil.which("bwrap"):
    print("bwrap is not installed: skipped")
    sys.exit(77)

with tempfile.TemporaryDirectory() as work:
    work = pathlib.Path(work)
    record = work / "record"
    # The program reports what it can see, then echoes its input. The
    # sessions of the user must be out of its reach: no AFTERGLOW_SOCKET, an
    # empty /run and /home, only the processes of the sandbox, no network.
    env = dict(os.environ, AFTERGLOW_SOCKET="/run/user/secret.sock", AFTERGLOW_PANE_ID="7")
    probe = ('echo "home=$HOME sock=${AFTERGLOW_SOCKET:-none} pane=${AFTERGLOW_PANE_ID:-none}'
             ' run=$(ls -A /run | wc -l) users=$(ls -A /home | wc -l)'
             ' procs=$(ls /proc | grep -c "^[0-9]")"; exec cat')
    result = run(["--steps", "40", "--seed", "3", "--record", str(record), "--", "sh", "-c", probe], env=env)
    check(result.returncode == 0 and b"all checks passed" in result.stdout, (result.returncode, result.stderr))
    files = sorted(record.iterdir())
    check(files and files[0].name.startswith("3-0001-80x24"), files)
    seen = files[0].read_bytes()
    check(b"home=/tmp/home sock=none pane=none run=0 users=0" in seen, seen[:200])
    procs = int(seen.split(b"procs=")[1].split()[0])
    check(procs <= 5, seen[:200])

    # The home directory that --home gives is the program's, and stays.
    home = work / "home"
    home.mkdir()
    result = run(["--steps", "5", "--home", str(home), "--", "sh", "-c", "echo made > made.txt; exec cat"])
    check(result.returncode == 0, result.stderr)
    check((home / "made.txt").read_text() == "made\n", list(home.iterdir()))

    # A program that cannot start ends the run with status 2.
    result = run(["--steps", "50", "--", "/nonexistent/program"])
    check(result.returncode == 2 and b"ends at once" in result.stderr, (result.returncode, result.stderr))

print("ok")
