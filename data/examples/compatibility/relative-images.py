#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Kitty relative placement demo: the smaller image follows its moving parent."""
import base64
import shutil
import sys
import time

def graphics(keys, data=b''):
    sys.stdout.write('\033_G' + keys + ',q=2' + (';' + base64.b64encode(data).decode() if data else '') + '\033\\')

if not sys.stdout.isatty():
    raise SystemExit('Run inside a terminal.')
try:
    sys.stdout.write('\033[?1049h\033[?25l')
    graphics('a=t,f=32,s=1,v=1,i=10001', bytes([50, 170, 240, 255]))
    graphics('a=t,f=32,s=1,v=1,i=10002', bytes([210, 85, 200, 255]))
    graphics('a=p,i=10001,p=1,c=8,r=4,C=1')
    graphics('a=p,i=10002,p=1,c=4,r=2,P=10001,Q=1,H=6,V=3')
    for frame in range(100):
        width, height = shutil.get_terminal_size()
        column = 1 + frame % max(1, width - 12)
        sys.stdout.write(f'\033[{min(3, max(1, height-6))};{column}H')
        graphics('a=p,i=10001,p=1,c=8,r=4,C=1')
        sys.stdout.flush()
        time.sleep(.05)
finally:
    graphics('a=d,d=I,i=10001')
    graphics('a=d,d=I,i=10002')
    sys.stdout.write('\033[?25h\033[?1049l')
    sys.stdout.flush()
