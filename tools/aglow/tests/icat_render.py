#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""aglow icat through afterglow_renderpng: what aglow writes is what a terminal shows.

Usage: icat_render.py AGLOW AFTERGLOW_RENDERPNG

The images are made here, so the test needs no image library: a still PNG
(sent as it is), the same PNG mirrored (decoded and sent as pixels with
zlib), the PNG scaled into a --place rectangle, Unicode placeholders with
and without --place, a header too large for a terminal (refused before a
decode), and an animated GIF whose frames are shown one at a time.

afterglow_renderpng draws with Vulkan, which needs a Wayland display: Qt's
offscreen platform has no Vulkan. Without a display or a Vulkan device the
test exits with 77, which CTest reports as skipped. aglow runs with
--passthrough=none and without TMUX, so that it never changes the settings
of a tmux that runs the test. afterglow_renderpng runs on the wayland
platform with the generic platform theme, so that the desktop's theme plugin
(and the session bus and portal that it opens) is not loaded.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

RED, GREEN, BLUE = (255, 0, 0), (0, 255, 0), (0, 0, 255)
COLUMNS, ROWS = 40, 12


def check(condition, *detail):
    """Raises AssertionError when condition is false. Not assert: python -O must not turn the checks off."""
    if not condition:
        raise AssertionError(*detail)


def png(width, height, colour_at):
    rows = b"".join(b"\0" + b"".join(bytes(colour_at(x, y)) + b"\xff" for x in range(width)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


def gif(width, height, colours):
    """One solid frame for each colour, 0.2 s apart, looping."""
    palette = b"".join(bytes(c) for c in colours) + b"\0\0\0" * (4 - len(colours))
    out = b"GIF89a" + struct.pack("<HHBBB", width, height, 0xF1, 0, 0) + palette
    out += b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00"
    for index in range(len(colours)):
        out += b"\x21\xf9\x04\x00" + struct.pack("<H", 20) + b"\x00\x00"
        out += b"\x2c" + struct.pack("<HHHHB", 0, 0, width, height, 0)
        # LZW with 2-bit pixels: a clear code before every two pixels keeps
        # every code 3 bits wide, so no table needs to be built here.
        bits, value, data = 0, 0, bytearray()

        def put(code):
            nonlocal bits, value
            value |= code << bits
            bits += 3
            while bits >= 8:
                data.append(value & 255)
                value >>= 8
                bits -= 8
        pixels = width * height
        for at in range(0, pixels, 2):
            put(4)
            put(index)
            if at + 1 < pixels:
                put(index)
        put(5)
        if bits:
            data.append(value & 255)
        out += b"\x02" + b"".join(bytes([len(data[i:i + 255])]) + data[i:i + 255] for i in range(0, len(data), 255))
        out += b"\x00"
    return out + b"\x3b"


def read_png(path):
    """The pixels of an 8-bit RGB or RGBA PNG, as rows of RGB tuples."""
    data = Path(path).read_bytes()
    check(data.startswith(b"\x89PNG"), path)
    at, idat = 8, b""
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            check(depth == 8 and colour in (2, 6), "unexpected PNG format", depth, colour)
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    channels = 4 if colour == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        rows.append([tuple(line[x * channels:x * channels + 3]) for x in range(width)])
        previous = line
    return rows


def near(colour, want, tolerance=40):
    return all(abs(a - b) <= tolerance for a, b in zip(colour, want))


SKIP = 77


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: icat_render.py AGLOW AFTERGLOW_RENDERPNG")
    aglow, renderer = sys.argv[1], sys.argv[2]
    if not os.environ.get("WAYLAND_DISPLAY"):
        print("skipped: afterglow_renderpng needs a Wayland display for Vulkan, and WAYLAND_DISPLAY is not set")
        sys.exit(SKIP)
    environment = {key: value for key, value in os.environ.items() if key != "TMUX"}
    render_environment = dict(environment, QT_QPA_PLATFORM="wayland", QT_QPA_PLATFORMTHEME="generic")
    with tempfile.TemporaryDirectory(prefix="aglow-icat-") as temporary:
        root = Path(temporary)

        def render(name, stream):
            (root / (name + ".vt")).write_bytes(stream)
            screen = root / (name + "-screen.png")
            result = subprocess.run([renderer, "--cols", str(COLUMNS), "--rows", str(ROWS), str(root / (name + ".vt")),
                                     str(screen)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                    env=render_environment, timeout=120)
            check(result.returncode == 0, name, result.returncode, result.stderr.decode(errors="replace"))
            return read_png(screen)

        # The first render tells a missing Vulkan from a failure of the test.
        probe = subprocess.run([renderer, "--cols", str(COLUMNS), "--rows", str(ROWS), "-", str(root / "probe.png")],
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               env=render_environment, timeout=120)
        if probe.returncode != 0 and b"Vulkan" in probe.stderr:
            print("skipped:", probe.stderr.decode(errors="replace").strip())
            sys.exit(SKIP)
        blank = render("blank", b"")
        cell_width, cell_height = len(blank[0]) // COLUMNS, len(blank) // ROWS
        window = f"{COLUMNS},{ROWS},{COLUMNS * cell_width},{ROWS * cell_height}"

        def run_icat(*arguments):
            return subprocess.run([aglow, "icat", "--use-window-size", window, "--transfer-mode=stream", "--stdin=no",
                                   "--passthrough=none", *arguments], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  env=environment, timeout=60)

        def icat(*arguments):
            result = run_icat(*arguments)
            check(result.returncode == 0, result.stderr.decode(errors="replace"))
            return result.stdout

        # Red on the left, blue on the right, a green square in the top left corner.
        (root / "still.png").write_bytes(png(8 * cell_width, 4 * cell_height, lambda x, y: (
            GREEN if x < cell_width and y < cell_height else RED if x < 4 * cell_width else BLUE)))
        # Centred: 16 columns of space before the 8 of the image.
        left = 16 * cell_width
        stream = icat(str(root / "still.png"))
        check(b"f=100" in stream, "a PNG that needs no change goes as it is")
        pixels = render("still", stream)
        check(near(pixels[cell_height // 2][left + cell_width // 2], GREEN), pixels[cell_height // 2][left + cell_width // 2])
        check(near(pixels[3 * cell_height][left + 2 * cell_width], RED))
        check(near(pixels[3 * cell_height][left + 6 * cell_width], BLUE))
        check(not near(pixels[3 * cell_height][left - cell_width], RED), "nothing left of the image")
        print("still PNG sent as it is: passed", flush=True)

        stream = icat("--mirror=horizontal", str(root / "still.png"))
        check(b"f=100" not in stream and b"o=z" in stream, "a changed image goes as pixels with zlib")
        pixels = render("mirrored", stream)
        check(near(pixels[cell_height // 2][left + 7 * cell_width + cell_width // 2], GREEN),
              pixels[cell_height // 2][left + 7 * cell_width + cell_width // 2], cell_width, cell_height)
        check(near(pixels[3 * cell_height][left + 2 * cell_width], BLUE))
        print("mirrored PNG decoded and sent with zlib: passed", flush=True)

        # Half the size: 4 columns and 2 rows at the top left corner of the screen.
        pixels = render("placed", icat("--place=4x2@0x0", "--align=left", str(root / "still.png")))
        check(near(pixels[cell_height // 4][cell_width // 4], GREEN))
        check(near(pixels[cell_height][cell_width + cell_width // 2], RED))
        check(near(pixels[cell_height][3 * cell_width], BLUE))
        check(not near(pixels[cell_height][5 * cell_width], BLUE), "the image stays in its place")
        print("--place scales the image into its rectangle: passed", flush=True)

        pixels = render("placeholder", icat("--unicode-placeholder", str(root / "still.png")))
        check(near(pixels[cell_height // 2][left + cell_width // 2], GREEN))
        check(near(pixels[3 * cell_height][left + 6 * cell_width], BLUE))
        print("Unicode placeholders: passed", flush=True)

        # Placed at the first column: no move before the cells.
        # The cursor ends on the first cell of the placement, and a placeholder
        # image is drawn under the cursor, as kitty draws it: move it away.
        pixels = render("placeholder-placed", icat("--unicode-placeholder", "--place=4x2@0x1", "--align=left",
                                                   str(root / "still.png")) + b"\x1b[%d;1H" % ROWS)
        check(near(pixels[cell_height + cell_height // 4][cell_width // 4], GREEN), pixels[cell_height + cell_height // 4][cell_width // 4])
        check(near(pixels[2 * cell_height][3 * cell_width], BLUE))
        print("Unicode placeholders with --place: passed", flush=True)

        # A header that names more pixels than a terminal takes is refused
        # before anything is decoded. The data after it is not an image.
        header = b"IHDR" + struct.pack(">IIBBBBB", 12000, 12000, 8, 6, 0, 0, 0)
        huge = (b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + header + struct.pack(">I", zlib.crc32(header))
                + png(1, 1, lambda x, y: RED)[33:])
        (root / "huge.png").write_bytes(huge)
        result = run_icat("--mirror=horizontal", str(root / "huge.png"))
        check(result.returncode == 1 and b"12000x12000" in result.stderr, result.returncode, result.stderr)
        check(b"\x1b_G" not in result.stdout, "nothing is sent for a refused image")
        print("an image larger than a terminal takes is refused: passed", flush=True)

        (root / "anim.gif").write_bytes(gif(4 * cell_width, 2 * cell_height, [RED, GREEN, BLUE]))
        stream = icat("--image-id=7", str(root / "anim.gif"))
        # The later chunks of a frame carry a=f too, with m= and no other key:
        # a frame is a chunk with a=f and more keys.
        frames = len(re.findall(rb"\x1b_Ga=f,(?!m=[01][,;])", stream))
        check(frames == 2, "two frames after the first", frames)
        middle = (18 * cell_width + 2 * cell_width, cell_height)
        for frame, colour in ((1, RED), (2, GREEN), (3, BLUE)):
            # Stop the animation on one frame: each frame must hold its own colour.
            shown = render(f"frame{frame}", stream + f"\x1b_Ga=a,i=7,s=1,c={frame},q=2\x1b\\".encode())
            check(near(shown[middle[1]][middle[0]], colour), frame, shown[middle[1]][middle[0]])
        print("animated GIF sent as frames: passed", flush=True)


if __name__ == "__main__":
    main()
