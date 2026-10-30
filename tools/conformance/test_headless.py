#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regression checks for the headless runner; pass its built executable."""
import pathlib
import shlex
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())


def run(args, code, expected=0):
    result = subprocess.run([binary, *args, "--", sys.executable, "-c", code],
                            capture_output=True, timeout=9)
    assert result.returncode == expected, (result.returncode, result.stderr)
    return result


for option in ("--cols", "--rows"):
    for value in ("0", "-1", "99999999999999999999", "no", "12junk"):
        run([option, value], "raise AssertionError('must not spawn')", 2)
run([], "raise SystemExit(7)", 7)
run([], "import os,tty; tty.setraw(0); os.write(1,b'\\x1b[6n'*1000000)", 2)

child = "import os,tty; tty.setraw(0); os.write(1,b'PROBE'); assert os.read(0,1)==b'x'"
responder = shlex.join([sys.executable, "-c",
                       "import pathlib,sys; assert 'PROBE' in pathlib.Path(sys.argv[1]).read_text(); print('x')"])
run(["--responder", responder], child)
with tempfile.TemporaryDirectory(prefix="afterglow ' quoted ") as directory:
    run(["--idle-dump", directory, "--responder", responder], child)
    assert list(pathlib.Path(directory).glob("*.txt"))
run(["--responder", "exit 7"], child, 2)
run(["--responder", shlex.join([sys.executable, "-c", "import os,time; os.close(1); time.sleep(30)"])], child, 2)
print("PASS: dimensions, exit status, bounded PTY replies, responder paths, failures and timeout")
