#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offer one file for local saving over a terminal, including an SSH session."""
import base64
import os
from pathlib import Path
import sys
if len(sys.argv) != 2 or not sys.stdout.isatty():
    raise SystemExit('Usage (inside a terminal): python3 download.py FILE')
path = Path(sys.argv[1])
with path.open('rb') as source:
    data = source.read(8 * 1024 * 1024 + 1)
if len(data) > 8 * 1024 * 1024:
    raise SystemExit('The file is larger than the 8 MiB download limit. Nothing was sent.')
name = base64.b64encode(os.fsencode(path.name)).decode()
sys.stdout.write(f'\033]1337;MultipartFile=name={name};size={len(data)}\007')
encoded = base64.b64encode(data).decode()
for at in range(0, len(encoded), 4096):
    sys.stdout.write('\033]1337;FilePart=' + encoded[at:at + 4096] + '\007')
sys.stdout.write('\033]1337;FileEnd\007')
sys.stdout.flush()
