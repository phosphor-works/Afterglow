#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded PTY output with a persistent first-row input acknowledgement."""
import json
import os
from pathlib import Path
import select
import sys
import termios
import time
import tty


def main():
    mode, result_path = sys.argv[1:]
    if mode not in ('input', 'input-load'):
        raise ValueError('Unknown workload')
    original = termios.tcgetattr(0)
    tty.setraw(0)
    os.set_blocking(1, False)
    height = os.get_terminal_size(1).lines
    pending = f'\x1b[2J\x1b[2;{height}r\x1b[{height};1H'.encode()
    row = b'\x1b[32moutput-load\x1b[0m ' + b'0123456789' * 5 + b'\r\n'
    chunk = row * 256
    rate = 4194304 if mode == 'input-load' else 0
    credit = 0.0
    previous = time.monotonic()
    first_ack = None
    first_bytes = 0
    written = 0
    input_buffer = b''
    replies = b''
    try:
        while True:
            now = time.monotonic()
            credit = min(65536, credit + (now - previous) * rate)
            previous = now
            if not pending:
                if replies:
                    pending, replies = replies, b''
                elif rate and credit >= len(chunk):
                    height = os.get_terminal_size(1).lines
                    pending = f'\x1b[2;{height}r\x1b[{height};1H'.encode() + chunk
                    credit -= len(pending)
            # With nothing to write and no output rate, sleep until input arrives
            # instead of waking the measured machine 500 times a second.
            idle = not pending and not rate
            readable, writable, _ = select.select([0], [1] if pending else [], [], None if idle else .002)
            if readable:
                data = os.read(0, 4096)
                if not data:
                    break
                input_buffer += data.replace(b'\r', b'\n')
                if len(input_buffer) > 65536:
                    raise ValueError('Input exceeds limit')
                while b'\n' in input_buffer:
                    line, input_buffer = input_buffer.split(b'\n', 1)
                    if len(line) != 9 or not line.startswith(b'ACK') or not line[3:].isdigit():
                        continue
                    replies += b'\x1b[s\x1b[1;1H\x1b[2K' + line + b'\x1b[u'
                    if first_ack is None:
                        first_ack, first_bytes = now, written
                    stats = {'last_ack': line.decode(), 'bytes_since_first_ack': written - first_bytes,
                             'seconds_since_first_ack': now - first_ack, 'target_bytes_per_second': rate}
                    path = Path(result_path + '.child.json')
                    temporary = path.with_suffix('.tmp')
                    temporary.write_text(json.dumps(stats))
                    temporary.replace(path)
            if writable and pending:
                try:
                    size = os.write(1, pending)
                    written += size
                    pending = pending[size:]
                except BlockingIOError:
                    pass
    finally:
        termios.tcsetattr(0, termios.TCSANOW, original)


if __name__ == '__main__':
    main()
