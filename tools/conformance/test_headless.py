#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regression checks for the headless runner; pass its built executable."""
import base64
import os
import pathlib
import shlex
import signal
import struct
import subprocess
import sys
import tempfile
import zlib

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

# The children check the reply with an exit status, not assert: they run
# with this environment, and PYTHONOPTIMIZE would drop an assert and its read.
child = "import os,tty; tty.setraw(0); os.write(1,b'PROBE'); raise SystemExit(os.read(0,1)!=b'x')"
# Prints nothing until PROBE is on screen: the runner may ask before any output arrives.
responder = shlex.join([sys.executable, "-c",
                        "import pathlib,sys; print('x' if 'PROBE' in pathlib.Path(sys.argv[1]).read_text() else '')"])
run(["--responder", responder], child)
with tempfile.TemporaryDirectory(prefix="afterglow ' quoted ") as directory:
    run(["--idle-dump", directory, "--responder", responder], child)
    check(list(pathlib.Path(directory).glob("*.txt")), "--idle-dump wrote no screen")
run(["--responder", "exit 7"], child, 2, stderr="responder failed")
run(["--responder", shlex.join([sys.executable, "-c", "print('x' * 2000000)"])], child, 2,
    stderr="responder output exceeded 1 MiB")
run(["--responder", shlex.join([sys.executable, "-c", "import os,time; os.close(1); time.sleep(30)"])], child, 2,
    stderr="responder timed out")
# --reply matches the screen as read: a wide character is one character.
run(["--reply", "中文=x"],
    "import os,tty; tty.setraw(0); os.write(1,'中文'.encode()); raise SystemExit(os.read(0,1)!=b'x')")


def filtered_png(width, height, pixel):
    """An RGBA PNG whose rows use the filters 0 to 4 in turn, which the decoder of --graphics undoes."""
    rows = [bytes(c for x in range(width) for c in pixel(x, y)) for y in range(height)]
    data, previous = b"", bytes(width * 4)
    for y, row in enumerate(rows):
        kind = y % 5
        line = bytearray()
        for i, value in enumerate(row):
            left = row[i - 4] if i >= 4 else 0
            up, corner = previous[i], previous[i - 4] if i >= 4 else 0
            if kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                predicted = left if pa <= pb and pa <= pc else up if pb <= pc else corner
            else:
                predicted = (0, left, up, (left + up) // 2)[kind]
            line.append((value - predicted) & 255)
        data += bytes([kind]) + line
        previous = row

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(data)) + chunk(b"IEND", b"")


# --graphics reports the images, placements and placeholder cells: a raw
# image placed at column 2, a PNG with every row filter, and a placeholder.
with tempfile.TemporaryDirectory() as directory:
    report = pathlib.Path(directory) / "graphics"
    gradient = filtered_png(7, 10, lambda x, y: (x * 30, y * 20, (x * y) % 256, 255))
    raw = base64.b64encode(bytes([1, 2, 3, 255, 4, 5, 6, 255])).decode()
    code = ("import os,sys; os.write(1, b'\\x1b[3G\\x1b_Ga=T,f=32,s=2,v=1,i=1,q=2;" + raw + "\\x1b\\\\'"
            " + b'\\x1b_Ga=t,f=100,i=2,q=2;' + sys.argv[1].encode() + b'\\x1b\\\\'"
            " + b'\\x1b_Ga=p,U=1,i=2,c=1,r=1,q=2\\x1b\\\\\\x1b[38;5;2m\\U0010eeee'.decode('unicode_escape').encode())")
    result = subprocess.run([binary, "--graphics", str(report), "--", sys.executable, "-c", code,
                             base64.b64encode(gradient).decode()], capture_output=True, timeout=9)
    check(result.returncode == 0, result.stderr)
    lines = report.read_text().splitlines()
    want = ["image 1 2x1 frames=1 animation=0",
            "frame 1 0 2x1 gap=0 topleft=010203ff centre=040506ff bottomright=040506ff",
            "image 2 7x10 frames=1 animation=0",
            "frame 2 0 7x10 gap=0 topleft=000000ff centre=5a640fff bottomright=b4b436ff",
            "placement 1 col=2 cols=0 rows=0 virtual=0",
            "placement 2 col=3 cols=1 rows=1 virtual=1",
            "placeholders 1"]
    check(lines == want, lines)
print("PASS: options, exit status, bounded PTY replies, responder paths, failures and timeouts, --graphics")
