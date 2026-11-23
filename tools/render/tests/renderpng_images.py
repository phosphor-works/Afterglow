#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""iTerm2 images in afterglow_renderpng: they decode as in a pane, and animations move on with --frames.
Usage: renderpng_images.py AFTERGLOW_RENDERPNG
The images are made here, so the test needs no image library: a netPBM
image, which a pane's decoders read, and an animated GIF whose frames show
one after the other at --fps. afterglow_renderpng draws with Vulkan, which
needs a Wayland display: without a display or a Vulkan device the test exits
with 77, which CTest reports as skipped. It runs on the wayland platform with
the generic platform theme, so that the desktop's theme plugin is not loaded.
"""
import base64
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

RED, GREEN, BLUE, MAGENTA = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 0, 255)
COLUMNS, ROWS = 20, 6
SKIP = 77


def check(condition, *detail):
    """Raises AssertionError when condition is false. Not assert: python -O must not turn the checks off."""
    if not condition:
        raise AssertionError(*detail)


def ppm(width, height, colour):
    return f"P6\n{width} {height}\n255\n".encode() + bytes(colour) * (width * height)


def gif(width, height, colours, centiseconds):
    """One solid frame for each colour, centiseconds apart, looping."""
    palette = b"".join(bytes(c) for c in colours) + b"\0\0\0" * (4 - len(colours))
    out = b"GIF89a" + struct.pack("<HHBBB", width, height, 0xF1, 0, 0) + palette
    out += b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00"
    for index in range(len(colours)):
        out += b"\x21\xf9\x04\x00" + struct.pack("<H", centiseconds) + b"\x00\x00"
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


def inline(image):
    """An iTerm2 inline image over 8 columns and 4 rows."""
    return b"\x1b]1337;File=inline=1;width=8;height=4;preserveAspectRatio=0:" + base64.b64encode(image) + b"\x07"


def pixel(path, x, y):
    """The RGB of one pixel of an 8-bit RGB or RGBA PNG."""
    data = Path(path).read_bytes()
    check(data.startswith(b"\x89PNG"), path)
    at, idat = 8, b""
    width = colour = None
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, _, depth, colour = struct.unpack(">IIBB", body[:10])
            check(depth == 8 and colour in (2, 6), "unexpected PNG format", depth, colour)
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    check(width is not None, "no IHDR chunk", path)
    channels = 4 if colour == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    previous = bytearray(stride)
    for row in range(y + 1):
        kind, line = raw[row * (stride + 1)], bytearray(raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)])
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
        previous = line
    return tuple(previous[x * channels:x * channels + 3])


def near(colour, want, tolerance=40):
    return all(abs(a - b) <= tolerance for a, b in zip(colour, want))


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: renderpng_images.py AFTERGLOW_RENDERPNG")
    renderer = sys.argv[1]
    if not os.environ.get("WAYLAND_DISPLAY"):
        print("skipped: afterglow_renderpng needs a Wayland display for Vulkan, and WAYLAND_DISPLAY is not set")
        sys.exit(SKIP)
    environment = dict(os.environ, QT_QPA_PLATFORM="wayland", QT_QPA_PLATFORMTHEME="generic")
    with tempfile.TemporaryDirectory(prefix="afterglow-renderpng-") as temporary:
        root = Path(temporary)

        def render(name, stream, *options):
            (root / (name + ".vt")).write_bytes(stream)
            result = subprocess.run([renderer, "--cols", str(COLUMNS), "--rows", str(ROWS), *options, str(root / (name + ".vt")),
                                     str(root / (name + ".png"))], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                    env=environment, timeout=120)
            return result

        # The first render tells a missing Vulkan from a failure of the test.
        probe = render("probe", b"")
        if probe.returncode != 0 and b"\nno Vulkan: " in b"\n" + probe.stderr:
            print("skipped:", probe.stderr.decode(errors="replace").strip())
            sys.exit(SKIP)
        check(probe.returncode == 0, probe.stderr.decode(errors="replace"))

        # The middle of the image's first cell row, two cells in.
        width, height = struct.unpack(">II", (root / "probe.png").read_bytes()[16:24])
        at = (2 * (width // COLUMNS), height // ROWS // 2)

        result = render("ppm", inline(ppm(16, 16, MAGENTA)))
        check(result.returncode == 0, result.stderr.decode(errors="replace"))
        shown = pixel(root / "ppm.png", *at)
        check(near(shown, MAGENTA), "a netPBM image shows as in a pane", shown)
        print("netPBM image: passed", flush=True)

        # 0.2 s a frame at 5 frames a second: each frame shows the next colour.
        result = render("gif", inline(gif(16, 16, [RED, GREEN, BLUE], 20)), "--frames", "3", "--fps", "5")
        check(result.returncode == 0, result.stderr.decode(errors="replace"))
        for frame, colour in enumerate((RED, GREEN, BLUE)):
            shown = pixel(root / f"gif-{frame:04d}.png", *at)
            check(near(shown, colour), "frame", frame, shown)
        print("animated GIF over --frames: passed", flush=True)


if __name__ == "__main__":
    main()
