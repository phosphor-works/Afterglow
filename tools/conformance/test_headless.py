#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regression checks for the headless runner; pass its built executable."""
import os
import pathlib
import shlex
import signal
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())


def check(condition, message):
    # Not assert: python -O would skip the checks.
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def run(args, code, expected=0, stderr=None):
    result = subprocess.run([binary, *args, "--", sys.executable, "-c", code],
                            capture_output=True, timeout=9)
    check(result.returncode == expected, (args, result.returncode, result.stderr))
    if stderr is not None:
        check(stderr.encode() in result.stderr, (args, result.stderr))
    return result


never = "raise AssertionError('must not spawn')"
for option in ("--cols", "--rows"):
    for value in ("0", "-1", "99999999999999999999", "no", "12junk"):
        run([option, value], never, 2)
run(["--cols", "1"], never, 2)  # the terminal core keeps two columns
for args in (["--bogus"], ["--responder"]):
    # An unknown option, or a value option at the end, is not run as the command.
    result = subprocess.run([binary, *args], capture_output=True, timeout=9)
    check(result.returncode == 2 and b"usage:" in result.stderr, (result.returncode, result.stderr))
run(["--reply", "a=b", "--responder", "true"], never, 2)
run([], "raise SystemExit(7)", 7)
# A last query before the program exits: the reply has no reader, which is not a failure.
run([], "import os; os.write(1, b'\\x1b[c')", 0)
# Replies the program never reads stop at the 1 MiB bound. The program stays
# alive, so the bound and not a closed pty ends the run.
run([], "import os,time,tty; tty.setraw(0); os.write(1,b'\\x1b[6n'*1000000); time.sleep(30)", 2,
    stderr="PTY reply queue exceeded 1 MiB")
# A background process that keeps the pty open (in its own session, so the
# hangup does not stop it) does not keep the runner waiting past the timeout.
with tempfile.TemporaryDirectory() as directory:
    pid_file = pathlib.Path(directory) / "pid"
    run([], "import pathlib,subprocess; p=subprocess.Popen(['sleep', '15'], start_new_session=True); "
            f"pathlib.Path({str(pid_file)!r}).write_text(str(p.pid))", 0)
    try:
        os.kill(int(pid_file.read_text()), signal.SIGTERM)  # do not leave it behind
    except (OSError, ValueError):
        pass

child = "import os,tty; tty.setraw(0); os.write(1,b'PROBE'); assert os.read(0,1)==b'x'"
# Prints nothing until PROBE is on screen: the runner may ask before any output arrives.
responder = shlex.join([sys.executable, "-c",
                        "import pathlib,sys; print('x' if 'PROBE' in pathlib.Path(sys.argv[1]).read_text() else '')"])
run(["--responder", responder], child)
with tempfile.TemporaryDirectory(prefix="afterglow ' quoted ") as directory:
    run(["--idle-dump", directory, "--responder", responder], child)
    check(list(pathlib.Path(directory).glob("*.txt")), "--idle-dump wrote no screen")
run(["--responder", "exit 7"], child, 2)
run(["--responder", shlex.join([sys.executable, "-c", "print('x' * 2000000)"])], child, 2,
    stderr="responder output exceeded 1 MiB")
run(["--responder", shlex.join([sys.executable, "-c", "import os,time; os.close(1); time.sleep(30)"])], child, 2)
# --reply matches the screen as read: a wide character is one character.
run(["--reply", "中文=x"], "import os,tty; tty.setraw(0); os.write(1,'中文'.encode()); assert os.read(0,1)==b'x'")
print("PASS: options, exit status, bounded PTY replies, responder paths, failures and timeouts")
