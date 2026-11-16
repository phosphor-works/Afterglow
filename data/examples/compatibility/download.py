#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offer one file for local saving over a terminal, including an SSH session.

The terminal asks before the data comes, and this program waits until the
answer: its writes block meanwhile. The file goes in parts of 3 KiB, which
read and send it with no limit on its size."""
import base64
import os
from pathlib import Path
import sys
if len(sys.argv) != 2 or not sys.stdout.isatty():
    raise SystemExit('Usage (inside a terminal): python3 download.py FILE')
path = Path(sys.argv[1])
size = path.stat().st_size
name = base64.b64encode(os.fsencode(path.name)).decode()
out = sys.stdout.buffer
out.write(f'\033]1337;MultipartFile=name={name};size={size}\007'.encode())
with path.open('rb') as source:
    # A multiple of 3 bytes: each part is whole base64.
    while part := source.read(3072):
        out.write(b'\033]1337;FilePart=' + base64.b64encode(part) + b'\007')
out.write(b'\033]1337;FileEnd\007')
out.flush()
