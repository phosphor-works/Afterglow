#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tiny ASCII multi-edit client using native Kitty multiple cursors.
Type to edit three values together; Backspace deletes; Tab cycles cursor colors;
Esc or Ctrl-C exits. Colors are shared by all extra cursors.
Also runs in Kitty with multiple-cursor protocol support.
"""
import os
import shutil
import sys
import termios
import tty
if not sys.stdin.isatty() or not sys.stdout.isatty():
    raise SystemExit('Run interactively inside a terminal.')
old = termios.tcgetattr(sys.stdin)
value = ''
color_mode = 0
colors = (
    ('RGB', '\033[>40;2:80:220:200 q\033[>30;2:12:24:36 q'),
    ('palette', '\033[>40;5:13 q\033[>30;5:0 q'),
    ('reverse cell', '\033[>40;1 q\033[>30;0 q'),
    ('main cursor', '\033[>40;0 q\033[>30;0 q'),
)
try:
    tty.setraw(sys.stdin)
    sys.stdout.write('\033[?1049h\033[?25l\033[6 q')
    while True:
        width, height = shutil.get_terminal_size()
        value = value[:max(0, width - 14)]
        name, sequence = colors[color_mode]
        sys.stdout.write(sequence)
        heading = f'Multiple cursors [{name}]: type, Backspace, Tab colors, Esc'
        sys.stdout.write('\033[H' + heading[:max(0, width - 1)] + '\033[K')
        sys.stdout.write('\033[>0;4 q')
        for row, label in enumerate(('primary', 'second ', 'third  '), 3):
            if row > height: break
            sys.stdout.write(f'\033[{row};1H{label} = {value}\033[K')
            shape = (1, 2, 3)[row - 3]  # block, beam and underline
            sys.stdout.write(f'\033[>{shape};2:{row}:{11 + len(value)} q')
        sys.stdout.flush()
        char = os.read(sys.stdin.fileno(), 1)
        if char in (b'\x1b', b'\x03', b'\x04'): break
        if char == b'\t':
            color_mode = (color_mode + 1) % len(colors)
        elif char in (b'\x7f', b'\x08'): value = value[:-1]
        elif b' ' <= char <= b'~': value += char.decode('ascii')
finally:
    sys.stdout.write('\033[>0;4 q\033[>30;0 q\033[>40;0 q\033[0 q\033[?25h\033[?1049l')
    sys.stdout.flush()
    termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old)
