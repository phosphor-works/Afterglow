#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offer one file for local saving over a terminal, including an SSH session.

The terminal asks before the data comes. This program waits for the answer,
because its writes block until then. The file goes in parts of 3 KiB, so a
file of any size goes without being read whole."""
import base64
import os
from pathlib import Path
import stat
import sys
if len(sys.argv) != 2 or not sys.stdout.isatty():
    raise SystemExit('Usage (inside a terminal): python3 download.py FILE')
path = Path(sys.argv[1])
# Opened before anything is offered: a file that cannot be read offers nothing.
try:
    source = path.open('rb')
except OSError as error:
    raise SystemExit(f'download.py: {error}')
out = sys.stdout.buffer
with source:
    info = os.fstat(source.fileno())
    if not stat.S_ISREG(info.st_mode):
        raise SystemExit(f'download.py: {path} is not a regular file')
    # The size of the open file: a file that grows meanwhile sends no more
    # than it offered, and one that shrinks ends short, which the terminal
    # reports as incomplete.
    size = info.st_size
    name = base64.b64encode(os.fsencode(path.name)).decode()
    out.write(f'\033]1337;MultipartFile=name={name};size={size}\007'.encode())
    try:
        left = size
        # A multiple of 3 bytes: each part is whole base64.
        while left > 0 and (part := source.read(min(3072, left))):
            out.write(b'\033]1337;FilePart=' + base64.b64encode(part) + b'\007')
            left -= len(part)
    finally:
        # Also after a read error: the terminal ends the file and frees it.
        out.write(b'\033]1337;FileEnd\007')
        out.flush()
