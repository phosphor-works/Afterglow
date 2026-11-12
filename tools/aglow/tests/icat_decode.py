#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The pixels that aglow icat sends for crafted images, read back from its graphics commands.

Usage: icat_decode.py AGLOW

Each image is built here, byte by byte, to hit one case of the decoders
(libs/afterglow-image) and of aglow around them: a GIF canvas that starts
transparent, GIF disposal and graphic control extension semantics, the
two_back read of GHSL-2023-145, the frame limit and its warning, frames that
do not all fit in the 320 MiB budget of an image and its warning (and frames
that fit exactly), --loop 0, which decodes one frame, the delays of frames as
kitty takes them, a frame far larger than its canvas, a frame partly off the
canvas, interlaced frames, the least code size, LZW without a leading clear
code, a GIF damaged after its frames, a WebP and its animation, BMP gaps and
top-down rows, invalid PNG depths, a valid PNG sent as it is and a damaged one
that is not, the longest side, the EXIF orientation of a JPEG, a PNG and a
WebP, a JPEG without Huffman tables (as Motion JPEG), and a corrupt file,
which the decoder child must turn into an error and not a crash of aglow. It
also checks that a subdirectory that cannot be read leaves out only itself,
that a named pipe in a directory is no image, that aglow stops when its
output closes, and that a signal to aglow ends a decode that does not end,
and the decoder child with it.

It exits with 77 (skipped) after the other cases when it runs as a user that
reads a directory of mode 0, such as root: the subdirectory case cannot run.

aglow runs with --passthrough=none and without TMUX, so that it never changes
the settings of a tmux that runs the test.
"""
import base64
import os
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path


def check(condition, *detail):
    """Raises AssertionError when condition is false. Not assert: python -O must not turn the checks off."""
    if not condition:
        raise AssertionError(*detail)


# Images.

def lzw(indices, min_code=2, lead_clear=True):
    """GIF image data that never grows its code size: a clear code before the table would grow."""
    clear = 1 << min_code
    size = min_code + 1
    room = (1 << size) - (clear + 2)
    codes = [clear] if lead_clear else []
    count = 0
    for index in indices:
        codes.append(index)
        count += 1
        if count >= room:
            codes.append(clear)
            count = 0
    codes.append(clear + 1)
    bits = 0
    held = 0
    data = bytearray()
    for code in codes:
        bits |= code << held
        held += size
        while held >= 8:
            data.append(bits & 255)
            bits >>= 8
            held -= 8
    if held:
        data.append(bits & 255)
    out = bytearray([min_code])
    for at in range(0, len(data), 255):
        chunk = data[at:at + 255]
        out += bytes([len(chunk)]) + chunk
    return bytes(out + b"\0")


def lzw_long(count, min_code=2):
    """GIF image data of index 0 only: each code one pixel longer than the one before, up to a full
    table, then COUNT codes of the longest string (about 4090 pixels each)."""
    clear = 1 << min_code
    size = min_code + 1
    available = clear + 2
    codes = [(clear, size), (0, size)]
    while available < 4096:
        codes.append((available, size))
        available += 1
        if available == 1 << size and size < 12:
            size += 1
    codes += [(4095, size)] * count
    codes.append((clear + 1, size))
    bits = 0
    held = 0
    data = bytearray()
    for code, width in codes:
        bits |= code << held
        held += width
        while held >= 8:
            data.append(bits & 255)
            bits >>= 8
            held -= 8
    if held:
        data.append(bits & 255)
    out = bytearray([min_code])
    for at in range(0, len(data), 255):
        chunk = data[at:at + 255]
        out += bytes([len(chunk)]) + chunk
    return bytes(out + b"\0")


PALETTE = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]


def gif(width, height, background, frames, trailer=True, raster=None):
    out = bytearray(b"GIF89a" + struct.pack("<HHBBB", width, height, 0x80 | 1, background, 0))
    for colour in PALETTE:
        out += bytes(colour)
    for frame in frames:
        left, top, w, h = frame["rect"]
        if "gce" in frame:
            disposal, delay, transparent = frame["gce"]
            packed = disposal << 2 | (1 if transparent is not None else 0)
            out += b"\x21\xf9\x04" + struct.pack("<BHB", packed, delay, transparent or 0) + b"\0"
        out += b"\x2c" + struct.pack("<HHHHB", left, top, w, h, 0x40 if frame.get("interlaced") else 0)
        out += raster if raster is not None else lzw(frame["indices"])
    if trailer:
        out += b"\x3b"
    return bytes(out)


def bmp(width, rows, top_down=False, gap=0):
    """24-bit BMP with a 40-byte header, GAP bytes between the header and the pixels. ROWS are top first."""
    pad = (-(width * 3)) & 3
    stored = rows if top_down else list(reversed(rows))
    pixels = b"".join(b"".join(bytes((b, g, r)) for (r, g, b) in row) + b"\0" * pad for row in stored)
    offset = 14 + 40 + gap
    header = b"BM" + struct.pack("<IHHI", offset + len(pixels), 0, 0, offset)
    height = -len(rows) if top_down else len(rows)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 0, 0, 0, 0)
    return header + info + b"\xaa" * gap + pixels


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(width, height, depth, colour, raw, extra=b""):
    """A PNG with EXTRA chunks before its image data."""
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, colour, 0, 0, 0))
            + extra + png_chunk(b"IDAT", zlib.compress(raw)) + png_chunk(b"IEND", b""))


def tiff_orientation(value):
    """A TIFF block of one IFD with the EXIF orientation VALUE."""
    return b"MM\x00\x2a" + struct.pack(">I", 8) + struct.pack(">H", 1) + struct.pack(">HHIHH", 0x0112, 3, 1, value, 0) + b"\0\0\0\0"


# 2 by 2 lossless WebP, red (Pillow): AfterglowImage::TestFiles::Webp of libs/afterglow-image/tests.
WEBP = bytes.fromhex(
    "524946461c000000574542505650384c0f0000002f014000000710fd8ffe0722a2ff0100")
# 4 by 4 lossless WebP animation, two frames of 100 ms: red, then blue (Pillow): TestFiles::WebpAnimated.
WEBP_ANIMATED = bytes.fromhex(
    "524946468400000057454250565038580a00000002000000030000030000414e494d06000000000000000000414e4d462800"
    "0000000000000000030000030000640000025650384c0f0000002f03c000000710fd8ffe0722a2ff0100414e4d4628000000"
    "000000000000030000030000640000005650384c0f0000002f03c000000710d1fffe0722a2ff0100")
# 2 by 1 lossless WebP, red then blue, with an EXIF chunk of orientation 6 (Pillow).
WEBP_TURNED = bytes.fromhex(
    "524946465200000057454250565038580a000000080000000100000000005650384c110000002f010000000f10f3bffff31f0e"
    "32a2ff0100455849461a0000004d4d002a00000008000101120003000000010006000000000000")


def webp_frame_times(data, *milliseconds):
    """DATA, a WebP animation, with the times of its frames in turn."""
    out = bytearray(data)
    at = 0
    for time_ms in milliseconds:
        at = out.index(b"ANMF", at) + 8
        out[at + 12:at + 15] = time_ms.to_bytes(3, "little")
    return bytes(out)


def png_of_rows(width, height, row):
    """An 8-bit RGB PNG of HEIGHT copies of ROW (its filter byte included), compressed one row at a time:
    the raw rows of a large image never sit in memory at once."""
    packer = zlib.compressobj()
    data = b"".join(packer.compress(row) for _ in range(height)) + packer.flush()
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + png_chunk(b"IDAT", data) + png_chunk(b"IEND", b""))


def jpeg_rotated(tiff=None):
    """A grey baseline JPEG, 8 pixels wide and 16 high, with EXIF orientation 6 (turn it a quarter clockwise),
    or with the TIFF block TIFF. Its top block is 128 grey, its bottom block lighter (about 159)."""
    def segment(marker, body):
        return b"\xff" + bytes([marker]) + struct.pack(">H", len(body) + 2) + body
    exif = segment(0xE1, b"Exif\0\0" + (tiff_orientation(6) if tiff is None else tiff))
    quant = segment(0xDB, b"\x00" + bytes([8] * 64))
    frame = segment(0xC0, struct.pack(">BHHB", 8, 16, 8, 1) + b"\x01\x11\x00")
    # DC: symbol 0 (no change) is "0", symbol 5 (a 5-bit difference) is "10". AC: end of block is "0".
    dc = segment(0xC4, b"\x00" + bytes([1, 1] + [0] * 14) + b"\x00\x05")
    ac = segment(0xC4, b"\x10" + bytes([1] + [0] * 15) + b"\x00")
    scan = segment(0xDA, b"\x01\x01\x00\x00\x3f\x00")
    # Block 1: DC "0", EOB "0". Block 2: DC "10" + 31 as "11111", EOB "0". Padded with ones.
    data = bytes([0b00101111, 0b10111111])
    return b"\xff\xd8" + exif + quant + frame + dc + ac + scan + data + b"\xff\xd9"


def jpeg_without_tables():
    """A grey baseline JPEG of one block without a DHT segment, as a Motion JPEG frame: its scan is coded
    with the tables of Annex K of the JPEG standard. It is about 159 grey."""
    def segment(marker, body):
        return b"\xff" + bytes([marker]) + struct.pack(">H", len(body) + 2) + body
    quant = segment(0xDB, b"\x00" + bytes([8] * 64))
    frame = segment(0xC0, struct.pack(">BHHB", 8, 8, 8, 1) + b"\x01\x11\x00")
    scan = segment(0xDA, b"\x01\x01\x00\x00\x3f\x00")
    # DC category 5 is "110" in Annex K, then 31 as "11111". The AC end of block is "1010". Padded with ones.
    data = bytes([0b11011111, 0b10101111])
    return b"\xff\xd8" + quant + frame + scan + data + b"\xff\xd9"


def children(pid):
    """The processes whose parent is PID."""
    found = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            stat = Path(f"/proc/{entry}/stat").read_text()
        except OSError:
            continue
        if stat.rsplit(")", 1)[1].split()[1] == str(pid):
            found.append(int(entry))
    return found


def limits(pid):
    """The lines of /proc/PID/limits that the decoder child sets, or None when PID is gone."""
    try:
        lines = Path(f"/proc/{pid}/limits").read_text().splitlines()
    except OSError:
        return None
    return [line for line in lines if line.startswith(("Max cpu time", "Max address space"))]


def alive(pid):
    """Whether PID runs or is stopped: not a zombie, and not gone."""
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] not in ("Z", "X")
    except OSError:
        return False


# What aglow sends.

ENVIRONMENT = {key: value for key, value in os.environ.items() if key != "TMUX"}


def run(aglow, path, *options):
    result = subprocess.run([aglow, "icat", "--transfer-mode=stream", "--stdin=no", "--fit=none", "--passthrough=none",
                             "--use-window-size=80,24,800,480", *options, str(path)],
                            capture_output=True, env=ENVIRONMENT, timeout=60)
    return result.returncode, result.stdout, result.stderr.decode(errors="replace")


def gaps(output):
    """The gap keys of the a=a commands in OUTPUT, which set the time of the first frame."""
    found = []
    for keys in re.findall(rb"\x1b_G([^;\x1b]*);?[^\x1b]*\x1b\\", output):
        fields = dict(field.split("=", 1) for field in keys.decode().split(",") if "=" in field)
        if fields.get("a") == "a" and "z" in fields:
            found.append(int(fields["z"]))
    return found


def transmissions(output, decode=True):
    """The images and frames in the graphics commands of OUTPUT: their keys and, with DECODE, their pixels."""
    images = []
    current = None
    for keys, payload in re.findall(rb"\x1b_G([^;\x1b]*);?([^\x1b]*)\x1b\\", output):
        fields = dict(field.split("=", 1) for field in keys.decode().split(",") if "=" in field)
        if current is not None and set(fields) <= {"m", "a", "q"} and fields.get("a", "f") == "f":
            current["data"] += payload
        elif fields.get("a") in ("T", "t", "f"):
            current = {"keys": fields, "data": payload}
            images.append(current)
        else:
            continue
        if fields.get("m", "0") == "0":
            current = None
    if not decode:
        return images
    for image in images:
        data = base64.b64decode(image["data"])
        if image["keys"].get("o") == "z":
            data = zlib.decompress(data)
        image["pixels"] = data
    return images


def pixels(image):
    """The pixels of one transmission, as (r, g, b, a) tuples."""
    data = image["pixels"]
    step = 3 if image["keys"].get("f") == "24" else 4
    out = []
    for at in range(0, len(data), step):
        px = tuple(data[at:at + step])
        out.append(px + (255,) if step == 3 else px)
    return out


SKIP = 77


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: icat_decode.py AGLOW")
    aglow = str(Path(sys.argv[1]).resolve())
    skipped = []
    with tempfile.TemporaryDirectory(prefix="aglow-icat-") as temporary:
        root = Path(temporary)

        def show(name, data, *options):
            path = root / name
            path.write_bytes(data)
            code, out, err = run(aglow, path, *options)
            return code, transmissions(out), err

        def show_raw(name, data, *options):
            path = root / name
            path.write_bytes(data)
            return run(aglow, path, *options)

        def not_crashed(err):
            """A refusal by the decoder, not a crash or a limit of its child."""
            return "crashed" not in err and "more memory" not in err and "longer than aglow allows" not in err

        red, green, blue, white, clear = (255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255), (0, 0, 0, 0)

        # The canvas of a GIF starts transparent, as in kitty and browsers: the background colour of the
        # global table does not fill what the first frame does not cover. The pixels are in RGB order.
        code, sent, err = show("background.gif", gif(2, 1, 2, [{"rect": (0, 0, 1, 1), "indices": [0]}]))
        check(code == 0 and len(sent) == 1, err, len(sent))
        check(pixels(sent[0]) == [red, clear], pixels(sent[0]))
        code, sent, err = show("background-0.gif", gif(2, 1, 0, [{"rect": (1, 0, 1, 1), "indices": [2]}]))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [clear, blue], err, sent and pixels(sent[0]))
        print("GIF canvas that starts transparent, in RGB order: passed", flush=True)

        # --background fills what is transparent. "#fff" is white, as in kitten icat and CSS (the
        # escape codes read it as f0f0f0), and OKLCH comes out in sRGB.
        for spec, under in (("#fff", (255, 255, 255, 255)), ("oklch(0.5 0.1 100)", (113, 100, 11, 255))):
            code, sent, err = show("background.gif", gif(2, 1, 2, [{"rect": (0, 0, 1, 1), "indices": [0]}]),
                                   "--background=" + spec)
            check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [red, under], spec, err, sent and pixels(sent[0]))
        print("--background as kitten icat reads it: passed", flush=True)

        # Disposal 3 restores the canvas from before the frame, 2 clears the frame's rectangle, and
        # a graphic control extension applies to the next frame only: the third frame's makes index 3
        # transparent, and the last frame, with none, paints index 3 white over the red pixel.
        frames = [
            {"rect": (0, 0, 2, 1), "indices": [0, 0], "gce": (1, 10, None)},
            {"rect": (0, 0, 1, 1), "indices": [1], "gce": (3, 10, None)},
            {"rect": (1, 0, 1, 1), "indices": [2], "gce": (2, 10, 3)},
            {"rect": (0, 0, 1, 1), "indices": [3]},
        ]
        code, sent, err = show("dispose.gif", gif(2, 1, 0, frames))
        check(code == 0, err)
        check([pixels(s) for s in sent] == [[red, red], [green, red], [red, blue], [white, clear]], [pixels(s) for s in sent])
        print("GIF disposal: passed", flush=True)

        # GHSL-2023-145: disposal 3 on the second frame read two frames back, before the heap block.
        frames = [
            {"rect": (0, 0, 4, 4), "indices": [0] * 16, "gce": (1, 1, None)},
            {"rect": (0, 0, 4, 4), "indices": [1] * 16, "gce": (3, 1, None)},
            {"rect": (0, 0, 1, 1), "indices": [2], "gce": (3, 1, None)},
        ]
        code, sent, err = show("two_back.gif", gif(4, 4, 0, frames))
        check(code == 0 and len(sent) == 3, err, len(sent))
        check(pixels(sent[2]) == [blue] + [red] * 15, pixels(sent[2]))
        print("GIF disposal 3 on the second frame: passed", flush=True)

        # More frames than the terminal keeps: the first frame only, which alone is red.
        def first_red(count):
            return [{"rect": (0, 0, 1, 1), "indices": [0 if i == 0 else 1 + i % 3], "gce": (1, 1, None)} for i in range(count)]
        code, sent, err = show("many.gif", gif(1, 1, 0, first_red(1100)))
        check(code == 0 and len(sent) == 1 and sent[0]["keys"]["a"] == "T", err, len(sent))
        check(pixels(sent[0]) == [red], pixels(sent[0]))
        check("the animation has more than 1024 frames, the most that a terminal keeps, so only its first frame shows"
              in err, err)
        print("GIF frame limit: passed", flush=True)

        # Frames that do not all fit in the 320 MiB that aglow allows for an image: a canvas of 4096x2048
        # holds 32 MiB, so ten frames fit and the eleventh does not. The file is a few hundred bytes, as
        # each frame paints one pixel: the first frame only, and a warning.
        # The first frame alone paints its pixel red, and the background (index 0) fills nothing: the
        # pixels compare as bytes, not as eight million tuples.
        code, sent, err = show("budget.gif", gif(4096, 2048, 0, first_red(11)))
        check(code == 0 and len(sent) == 1 and sent[0]["keys"]["a"] == "T", err, len(sent))
        # RGBA, as the canvas has transparent pixels.
        check(sent[0]["keys"].get("f", "32") == "32", sent[0]["keys"])
        check(sent[0]["pixels"] == bytes(red) + bytes(4096 * 2048 * 4 - 4), "not the first frame alone")
        check("do not all fit in the 320.0 MiB that aglow allows for it, so only its first frame shows" in err, err)
        sent = None
        # Ten frames fill the budget exactly: all of them go, with no warning. They are counted, not decoded.
        path = root / "budget-fits.gif"
        path.write_bytes(gif(4096, 2048, 0, first_red(10)))
        code, out, err = run(aglow, path)
        check(code == 0 and len(transmissions(out, decode=False)) == 10, err, len(transmissions(out, decode=False)))
        check("only its first frame shows" not in err, err)
        out = None
        print("GIF over the image budget: passed", flush=True)

        # --loop 0 shows the first frame: the others are not decoded, and that is no loss to warn about.
        code, sent, err = show("many-once.gif", gif(1, 1, 0, first_red(1100)), "--loop=0")
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [red] and not err, code, len(sent), err)
        print("--loop 0 decodes one frame: passed", flush=True)

        # Delays as kitty takes them. A GIF whose frames all have no delay plays at 100 ms a frame. Else
        # each frame keeps its own delay, and one without a delay is gapless: z=-1. The first frame gets
        # its time from a=a, the others from a=f.
        def two_frames(first, second):
            return gif(1, 1, 0, [{"rect": (0, 0, 1, 1), "indices": [0], "gce": (1, first, None)},
                                 {"rect": (0, 0, 1, 1), "indices": [1], "gce": (1, second, None)}])
        for name, delays, expected in (("zero", (0, 0), (100, 100)), ("mixed", (0, 5), (-1, 50)), ("short", (1, 2), (10, 20))):
            path = root / f"delays-{name}.gif"
            path.write_bytes(two_frames(*delays))
            code, out, err = run(aglow, path)
            frames = transmissions(out, decode=False)
            check(code == 0 and len(frames) == 2, name, err)
            check(gaps(out) == [expected[0]] and frames[1]["keys"].get("z") == str(expected[1]), name, gaps(out), frames[1]["keys"])
        # A WebP keeps the times of its frames, short ones too: 16 ms is 60 frames a second.
        path = root / "fast.webp"
        path.write_bytes(webp_frame_times(WEBP_ANIMATED, 16, 17))
        code, out, err = run(aglow, path)
        frames = transmissions(out, decode=False)
        check(code == 0 and len(frames) == 2 and gaps(out) == [16] and frames[1]["keys"].get("z") == "17", err, gaps(out),
              frames and frames[-1]["keys"])
        print("frame delays as kitty takes them: passed", flush=True)

        # A WebP, and a WebP animation, whose frames go as a=f.
        code, sent, err = show("still.webp", WEBP)
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [red] * 4, err, sent and pixels(sent[0]))
        code, sent, err = show("animated.webp", WEBP_ANIMATED)
        check(code == 0 and len(sent) == 2 and sent[0]["keys"]["a"] == "T" and sent[1]["keys"]["a"] == "f", err, len(sent))
        check(pixels(sent[0]) == [red] * 16 and pixels(sent[1]) == [blue] * 16, pixels(sent[0])[:1], pixels(sent[1])[:1])
        print("WebP and its animation: passed", flush=True)

        # A frame of 65535 pixels a side on a canvas of one, with data that spells strings of 4090
        # pixels: decoding stops once no pixel can land, in far less than the CPU limit of the child.
        # The stream is built before the clock starts: only aglow's decode is timed.
        huge = gif(1, 1, 0, [{"rect": (0, 0, 65535, 65535)}], raster=lzw_long(1000000))
        start = time.monotonic()
        code, sent, err = show("huge-frame.gif", huge)
        elapsed = time.monotonic() - start
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [red], err, sent and pixels(sent[0]))
        check(elapsed < 4, elapsed)
        # The same on a canvas of 2 by 2: the pixels that land are those of the first two rows.
        code, sent, err = show("huge-frame-2.gif", gif(2, 2, 0, [{"rect": (0, 0, 65535, 65535)}], raster=lzw_long(64)))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [red] * 4, err, sent and pixels(sent[0]))
        # A frame partly off the canvas, and an interlaced frame whose fourth row is below it.
        code, sent, err = show("offset.gif", gif(3, 3, 0, [{"rect": (1, 1, 4, 2), "indices": [0, 1, 2, 3, 1, 2, 3, 0]}]))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [clear] * 4 + [red, green] + [clear, green, blue], err,
              sent and pixels(sent[0]))
        code, sent, err = show("interlaced.gif", gif(1, 3, 0, [{"rect": (0, 0, 1, 4), "indices": [0, 1, 2, 3],
                                                                "interlaced": True}]))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [red, blue, green], err, sent and pixels(sent[0]))
        print("GIF frames larger than the canvas, and interlaced: passed", flush=True)

        # GIF89a takes 2 as the least code size: 1 is corrupt.
        code, sent, err = show("code-size-1.gif", gif(1, 1, 0, [{"rect": (0, 0, 1, 1)}], raster=lzw([1], min_code=1)))
        check(code == 1 and not sent and "the image data of the GIF is corrupt" in err and not_crashed(err), code, err)
        print("GIF least code size: passed", flush=True)

        # LZW data that starts without a clear code, and a frame larger than the screen, clipped.
        code, sent, err = show("noclear.gif", gif(2, 1, 0, [{"rect": (0, 0, 2, 1)}], raster=lzw([1, 2], lead_clear=False)))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [green, blue], err, sent and pixels(sent[0]))
        code, sent, err = show("clip.gif", gif(1, 1, 0, [{"rect": (0, 0, 2, 2), "indices": [1, 2, 3, 0]}]))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [green], err, sent and pixels(sent[0]))
        print("GIF without a leading clear code, and clipped: passed", flush=True)

        # Damage after the first frame keeps the frames before it, and says so.
        damaged = gif(1, 1, 0, [{"rect": (0, 0, 1, 1), "indices": [0]}, {"rect": (0, 0, 1, 1), "indices": [1]}], trailer=False) + b"\x99"
        code, sent, err = show("damaged.gif", damaged)
        check(code == 0 and len(sent) == 2 and "stops after frame 2" in err, code, len(sent), err)
        print("GIF damaged after its frames: passed", flush=True)

        # A BMP with bytes between its header and its pixels, and a top-down BMP.
        code, sent, err = show("gap.bmp", bmp(1, [[(10, 20, 30)]], gap=4))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [(10, 20, 30, 255)], err, sent and pixels(sent[0]))
        code, sent, err = show("top.bmp", bmp(1, [[(255, 0, 0)], [(0, 0, 255)]], top_down=True))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [red, blue], err, sent and pixels(sent[0]))
        print("BMP gap and top-down rows: passed", flush=True)

        # A PNG with a depth that its colour type does not take is refused; a valid one goes as it is.
        code, sent, err = show("bad.png", png(1, 1, 4, 2, b"\0\0\0"))
        check(code == 1 and not sent and "cannot decode the image" in err and not_crashed(err), code, err)
        valid = png(1, 1, 8, 2, b"\0\x01\x02\x03")
        code, sent, err = show("good.png", valid)
        check(code == 0 and len(sent) == 1 and sent[0]["keys"].get("f") == "100" and sent[0]["pixels"] == valid, err)
        print("PNG depths: passed", flush=True)

        # A PNG that the terminal could not decode does not go as it is: a wrong checksum and a cut file go to
        # the decoder, which says what is wrong. The terminal would say nothing. A file without its IEND
        # goes to the decoder too, which shows it, as browsers do.
        broken = bytearray(valid)
        broken[-20] ^= 0x55
        for name, data in (("checksum.png", bytes(broken)), ("cut.png", valid[:-20])):
            code, sent, err = show(name, data)
            check(code == 1 and not sent and "cannot decode the image" in err and not_crashed(err), name, code, err)
        code, sent, err = show("no-end.png", valid[:-12])
        check(code == 0 and len(sent) == 1 and sent[0]["keys"].get("f") != "100" and pixels(sent[0]) == [(1, 2, 3, 255)],
              code, err, sent and sent[0]["keys"])
        print("a damaged PNG is not sent as it is: passed", flush=True)

        # The longest side, from the header: 10,001 pixels is too long, also with all its pixels well
        # within the budget, and 10,000 decodes.
        code, sent, err = show("wide.png", png(10001, 1, 8, 2, b""))
        check(code == 1 and not sent and "10001x1" in err and "no side longer than 10000" in err, code, err)
        code, sent, err = show("widest.png", png_of_rows(10000, 1, b"\0" + b"\xff\0\0" * 10000), "--mirror=horizontal")
        check(code == 0 and len(sent) == 1, code, err)
        print("PNG past the longest side: passed", flush=True)

        # EXIF orientation 6: an image 8 wide and 16 high shows 16 wide and 8 high, its top on the right.
        code, sent, err = show("rotated.jpg", jpeg_rotated())
        check(code == 0 and len(sent) == 1, err)
        keys = sent[0]["keys"]
        check(keys.get("s") == "16" and keys.get("v") == "8", keys)
        row = pixels(sent[0])[:16]
        check(all(abs(px[0] - 159) <= 2 for px in row[:8]) and all(abs(px[0] - 128) <= 2 for px in row[8:]), row)
        # A TIFF block of 10 bytes whose IFD claims 65,535 entries: aglow reads none past the block, and the
        # image shows upright.
        code, sent, err = show("short-exif.jpg", jpeg_rotated(b"MM\x00\x2a" + struct.pack(">I", 8) + b"\xff\xff"))
        check(code == 0 and len(sent) == 1 and sent[0]["keys"].get("s") == "8" and sent[0]["keys"].get("v") == "16", code, err,
              sent and sent[0]["keys"])
        print("JPEG EXIF orientation: passed", flush=True)

        # The EXIF orientation of a PNG (an eXIf chunk) and of a WebP (an EXIF chunk), as kitten icat reads
        # them: an image 2 wide and 1 high, red then blue, turned a quarter clockwise, is 1 wide and 2 high
        # with red at the top. A PNG with an orientation is decoded and turned, not sent as it is.
        turned_png = png(2, 1, 8, 2, b"\0\xff\0\0\0\0\xff", png_chunk(b"eXIf", tiff_orientation(6)))
        for name, data in (("turned.png", turned_png), ("turned.webp", WEBP_TURNED)):
            code, sent, err = show(name, data)
            check(code == 0 and len(sent) == 1, name, err)
            keys = sent[0]["keys"]
            check(keys.get("f") != "100" and keys.get("s") == "1" and keys.get("v") == "2", name, keys)
            check(pixels(sent[0]) == [red, blue], name, pixels(sent[0]))
        print("PNG and WebP EXIF orientation: passed", flush=True)

        # A JPEG without DHT: libjpeg-turbo takes the Huffman tables of Annex K for it.
        code, sent, err = show("motion.jpg", jpeg_without_tables())
        check(code == 0 and len(sent) == 1 and all(abs(px[0] - 159) <= 2 for px in pixels(sent[0])), err,
              sent and pixels(sent[0]))
        print("JPEG without Huffman tables: passed", flush=True)

        # Corrupt data is an error from the decoder, and aglow goes on to the next image.
        corrupt = root / "corrupt.png"
        corrupt.write_bytes(valid[:41] + b"\xff" * 64)
        result = subprocess.run([aglow, "icat", "--transfer-mode=stream", "--stdin=no", "--fit=none", "--passthrough=none",
                                 "--use-window-size=80,24,800,480", "--mirror=horizontal", str(corrupt),
                                 str(root / "good.png")], capture_output=True, env=ENVIRONMENT, timeout=60)
        check(result.returncode == 1 and len(transmissions(result.stdout)) == 1, result.returncode, result.stderr)
        check(not_crashed(result.stderr.decode(errors="replace")), result.stderr)
        print("corrupt data: passed", flush=True)

        # A named pipe with an image name in a directory is no image file: aglow does not wait for a writer.
        pipes = root / "pipes"
        pipes.mkdir()
        (pipes / "a.png").write_bytes(valid)
        os.mkfifo(pipes / "b.png")
        code, out, err = run(aglow, pipes)
        check(code == 0 and len(transmissions(out)) == 1, code, err)
        print("a named pipe in a directory: passed", flush=True)

        # When its output closes, aglow stops: it does not decode the other images to fail on each.
        closed = root / "closed-output"
        closed.mkdir()
        for index in range(5):
            (closed / f"{index}.png").write_bytes(valid)
        reader, writer = os.pipe()
        os.close(reader)
        try:
            result = subprocess.run([aglow, "icat", "--transfer-mode=stream", "--stdin=no", "--fit=none", "--passthrough=none",
                                     "--use-window-size=80,24,800,480", str(closed)], stdout=writer, stderr=subprocess.PIPE,
                                    env=ENVIRONMENT, timeout=60)
        finally:
            os.close(writer)
        failures = result.stderr.decode(errors="replace").count("cannot write to the terminal")
        check(result.returncode == 1 and failures == 1, result.returncode, result.stderr)
        print("a closed output ends the images: passed", flush=True)

        # A subdirectory that cannot be read is left out with a message, and the images beside it show.
        tree = root / "tree"
        (tree / "closed").mkdir(parents=True)
        (tree / "image.png").write_bytes(valid)
        os.chmod(tree / "closed", 0)
        try:
            if not os.access(tree / "closed", os.R_OK):
                code, out, err = run(aglow, tree)
                check(code == 1 and len(transmissions(out)) == 1 and "closed" in err, code, err)
                print("a subdirectory that cannot be read: passed", flush=True)
            else:
                print("a subdirectory that cannot be read: skipped, this user reads a directory of mode 0",
                      flush=True)
                skipped.append("a subdirectory that cannot be read")
        finally:
            os.chmod(tree / "closed", 0o755)

        # A decode that never ends: the child is stopped. SIGTERM to aglow alone ends the wait and the
        # child, and when aglow is killed, the child goes with it.
        width = height = 8000
        (root / "big.png").write_bytes(png_of_rows(width, height, b"\0" * (1 + width * 3)))
        for stop in (signal.SIGTERM, signal.SIGKILL):
            process = subprocess.Popen([aglow, "icat", "--transfer-mode=stream", "--stdin=no", "--fit=none",
                                        "--passthrough=none", "--use-window-size=80,24,800,480", "--mirror=horizontal",
                                        str(root / "big.png")], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                       env=ENVIRONMENT)
            decoder = None
            try:
                # The child sets its limits after PR_SET_PDEATHSIG. Stopped before that, it would not go
                # with a killed aglow: wait until its limits differ from those of aglow.
                deadline = time.monotonic() + 30
                while decoder is None and process.poll() is None and time.monotonic() < deadline:
                    found = children(process.pid)
                    if found:
                        own = limits(found[0])
                        if own is not None and own != limits(process.pid):
                            decoder = found[0]
                    if decoder is None:
                        time.sleep(0.005)
                check(decoder is not None, "no decoder child that passed its setup")
                os.kill(decoder, signal.SIGSTOP)
                process.send_signal(stop)
                code = process.wait(timeout=10)
                deadline = time.monotonic() + 10
                while alive(decoder) and time.monotonic() < deadline:
                    time.sleep(0.01)
                check(not alive(decoder), "the decoder child outlived aglow")
                if stop == signal.SIGTERM:
                    check(code == 130 and b"interrupted" in process.stderr.read(), code)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                if decoder is not None and alive(decoder):
                    os.kill(decoder, signal.SIGKILL)
                process.stderr.close()
        print("a signal ends a decode: passed", flush=True)
    if skipped:
        print("skipped:", ", ".join(skipped), flush=True)
        sys.exit(SKIP)


if __name__ == "__main__":
    main()
