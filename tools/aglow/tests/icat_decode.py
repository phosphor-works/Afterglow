#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The pixels that aglow icat sends for crafted images, read back from its graphics commands.

Usage: icat_decode.py AGLOW

Each image is built here, byte by byte, to hit one decoder bug that aglow
fixed: GIF disposal, background and graphic control extension semantics, the
two_back read of GHSL-2023-145, the frame limit and its warning, frames that do
not all fit in the 320 MiB budget of an image and its warning (and frames that
fit exactly), a frame far larger than its canvas, a frame partly off the
canvas, interlaced frames, the least code size, LZW without a leading clear
code, a GIF damaged after its frames, 16-bit PNM, BMP gaps and top-down rows,
the direction and colour map of a TGA, a 16-bit PSD in RLE, HDR scanlines of
both kinds, invalid PNG depths and a valid PNG sent as it is, the EXIF
orientation of a JPEG, a JPEG without Huffman tables (as Motion JPEG), the
size of a PIC past the limit, and a corrupt file, which the decoder child must
turn into an error and not a crash of aglow. It also checks that a
subdirectory that cannot be read leaves out only itself, and that a signal to
aglow ends a decode that does not end, and the decoder child with it.

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


def png(width, height, depth, colour, raw):
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, colour, 0, 0, 0))
            + png_chunk(b"IDAT", zlib.compress(raw)) + png_chunk(b"IEND", b""))


def png_of_rows(width, height, row):
    """An 8-bit RGB PNG of HEIGHT copies of ROW (its filter byte included), compressed one row at a time:
    the raw rows of a large image never sit in memory at once."""
    packer = zlib.compressobj()
    data = b"".join(packer.compress(row) for _ in range(height)) + packer.flush()
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + png_chunk(b"IDAT", data) + png_chunk(b"IEND", b""))


def jpeg_rotated():
    """A grey baseline JPEG, 8 pixels wide and 16 high, with EXIF orientation 6 (turn it a quarter clockwise).
    Its top block is 128 grey, its bottom block lighter (about 159)."""
    def segment(marker, body):
        return b"\xff" + bytes([marker]) + struct.pack(">H", len(body) + 2) + body
    tiff = b"MM\x00\x2a" + struct.pack(">I", 8) + struct.pack(">H", 1) + struct.pack(">HHIHH", 0x0112, 3, 1, 6, 0) + b"\0\0\0\0"
    exif = segment(0xE1, b"Exif\0\0" + tiff)
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

        red, green, blue, white, clear = (255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255), (0, 0, 0, 0)

        # The background colour of the global table fills what the first frame does not cover, in RGB order.
        # Index 0, stb_image's rule, fills nothing: the canvas stays transparent there.
        code, sent, err = show("background.gif", gif(2, 1, 2, [{"rect": (0, 0, 1, 1), "indices": [0]}]))
        check(code == 0 and len(sent) == 1, err, len(sent))
        check(pixels(sent[0]) == [red, blue], pixels(sent[0]))
        code, sent, err = show("background-0.gif", gif(2, 1, 0, [{"rect": (0, 0, 1, 1), "indices": [1]}]))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [green, clear], err, sent and pixels(sent[0]))
        print("GIF background in RGB order: passed", flush=True)

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
        check(code == 1 and not sent and "corrupt" in err, code, err)
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

        # 16-bit PNM is big-endian, and samples scale from the maximum value.
        code, sent, err = show("deep.ppm", b"P6\n1 1\n1000\n" + struct.pack(">HHH", 1000, 500, 0))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [(255, 128, 0, 255)], err, sent and pixels(sent[0]))
        code, sent, err = show("low.ppm", b"P6 1 1 100\n" + bytes([100, 50, 0]))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [(255, 128, 0, 255)], err, sent and pixels(sent[0]))
        print("PNM depth and maximum value: passed", flush=True)

        # A BMP with bytes between its header and its pixels, and a top-down BMP.
        code, sent, err = show("gap.bmp", bmp(1, [[(10, 20, 30)]], gap=4))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [(10, 20, 30, 255)], err, sent and pixels(sent[0]))
        code, sent, err = show("top.bmp", bmp(1, [[(255, 0, 0)], [(0, 0, 255)]], top_down=True))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [red, blue], err, sent and pixels(sent[0]))
        print("BMP gap and top-down rows: passed", flush=True)

        # TGA: bit 4 of the descriptor mirrors the rows, and a colour map starts at its first index.
        header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, 2, 1, 24, 0x30)
        code, sent, err = show("mirrored.tga", header + bytes([0, 0, 255, 255, 0, 0]))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [blue, red], err, sent and pixels(sent[0]))
        header = struct.pack("<BBBHHBHHHHBB", 0, 1, 1, 10, 2, 24, 0, 0, 2, 1, 8, 0x20)
        code, sent, err = show("mapped.tga", header + bytes([0, 0, 255, 0, 255, 0, 11, 10]))
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [green, red], err, sent and pixels(sent[0]))
        print("TGA direction and colour map: passed", flush=True)

        # PSD: a 16-bit channel in RLE is a run of bytes, two for each sample.
        psd = b"8BPS" + struct.pack(">H", 1) + b"\0" * 6 + struct.pack(">HIIHH", 3, 1, 1, 16, 3) + struct.pack(">IIIH", 0, 0, 0, 1)
        psd += struct.pack(">HHH", 3, 3, 3) + b"".join(bytes([1]) + struct.pack(">H", v) for v in (0xFF00, 0x8000, 0))
        code, sent, err = show("deep.psd", psd)
        check(code == 0 and len(sent) == 1 and pixels(sent[0]) == [(255, 128, 0, 255)], err, sent and pixels(sent[0]))
        print("PSD 16-bit RLE: passed", flush=True)

        # HDR: a flat scanline after a run-length encoded one is the second row, not the first.
        row0 = bytes([2, 2, 0, 8]) + b"".join(bytes([128 + 8, v]) for v in (128, 64, 0, 129))
        hdr = b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 8\n" + row0 + bytes([64, 128, 0, 129]) * 8
        code, sent, err = show("mixed.hdr", hdr)
        rows = pixels(sent[0]) if code == 0 and sent else []
        check(len(rows) == 16 and rows[0][0] > rows[0][1] and rows[8][1] > rows[8][0], err, rows)
        print("HDR scanlines of both kinds: passed", flush=True)

        # A PNG with a depth that its colour type does not take is refused; a valid one goes as it is.
        code, sent, err = show("bad.png", png(1, 1, 4, 2, b"\0\0\0"))
        check(code == 1 and not sent, code, err)
        valid = png(1, 1, 8, 2, b"\0\x01\x02\x03")
        code, sent, err = show("good.png", valid)
        check(code == 0 and len(sent) == 1 and sent[0]["keys"].get("f") == "100" and sent[0]["pixels"] == valid, err)
        print("PNG depths: passed", flush=True)

        # EXIF orientation 6: an image 8 wide and 16 high shows 16 wide and 8 high, its top on the right.
        code, sent, err = show("rotated.jpg", jpeg_rotated())
        check(code == 0 and len(sent) == 1, err)
        keys = sent[0]["keys"]
        check(keys.get("s") == "16" and keys.get("v") == "8", keys)
        row = pixels(sent[0])[:16]
        check(all(abs(px[0] - 159) <= 2 for px in row[:8]) and all(abs(px[0] - 128) <= 2 for px in row[8:]), row)
        print("JPEG EXIF orientation: passed", flush=True)

        # A JPEG without DHT takes the Huffman tables of Annex K, as libjpeg does.
        code, sent, err = show("motion.jpg", jpeg_without_tables())
        check(code == 0 and len(sent) == 1 and all(abs(px[0] - 159) <= 2 for px in pixels(sent[0])), err,
              sent and pixels(sent[0]))
        print("JPEG without Huffman tables: passed", flush=True)

        # A PIC past the longest side is too large, with its size, and not an unknown file.
        pic = b"\x53\x80\xf6\x34" + b"\0" * 84 + b"PICT" + struct.pack(">HH", 20000, 20000) + b"\0" * 8 + bytes([0, 8, 0, 0xF0])
        code, sent, err = show("huge.pic", pic)
        check(code == 1 and not sent and "20000x20000" in err, code, err)
        print("PIC past the size limit: passed", flush=True)

        # Corrupt data is an error from the decoder, and aglow goes on to the next image.
        corrupt = root / "corrupt.png"
        corrupt.write_bytes(valid[:41] + b"\xff" * 64)
        result = subprocess.run([aglow, "icat", "--transfer-mode=stream", "--stdin=no", "--fit=none", "--passthrough=none",
                                 "--use-window-size=80,24,800,480", "--mirror=horizontal", str(corrupt),
                                 str(root / "good.png")], capture_output=True, env=ENVIRONMENT, timeout=60)
        check(result.returncode == 1 and len(transmissions(result.stdout)) == 1, result.returncode, result.stderr)
        print("corrupt data: passed", flush=True)

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
