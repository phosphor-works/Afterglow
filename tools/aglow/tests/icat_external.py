#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""aglow icat with the programs that it runs: ImageMagick and curl.

Usage: icat_external.py AGLOW

ImageMagick: an image of each format that aglow sends to magick (AVIF, HEIC,
JPEG XL, QOI, SVG, TGA, ICO, CUR and a run-length BMP), made by magick here.
A file whose name ImageMagick would read as a coder or a file list shows as
its data says. Data of a format outside the list (MVG) is refused without
magick. An SVG that names a local image shows without it. --engine builtin
and a PATH without magick refuse with a message, and the private directory
of each run goes.

curl: images from a local HTTP server that the test runs: a PNG, a redirect
to it, a redirect to a file: URL, which must fail, a file over the limit, a
missing file, and a PATH without curl.

It exits with 77 (skipped) after the other cases when magick or curl is not
installed, or magick cannot write a format, naming what it left out.
"""

import http.server
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from icat_decode import check, pixels, transmissions  # noqa: E402

SKIP = 77
COLOUR = (200, 100, 50)
PROXIES = {"http_proxy", "https_proxy", "ftp_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "FTP_PROXY", "ALL_PROXY"}


def environment(**extra):
    env = {key: value for key, value in os.environ.items() if key != "TMUX" and key not in PROXIES}
    env.update(extra)
    return env


def run(aglow, target, *options, env=None):
    """aglow icat on TARGET, a path or a URL, decoded to pixels (--mirror makes it decode a PNG too)."""
    result = subprocess.run([aglow, "icat", "--transfer-mode=stream", "--stdin=no", "--fit=none", "--passthrough=none",
                             "--use-window-size=80,24,800,480", "--mirror=horizontal", *options, str(target)],
                            capture_output=True, env=env or environment(), timeout=120)
    return result.returncode, transmissions(result.stdout), result.stderr.decode(errors="replace")


def near(px, want, tolerance):
    return all(abs(a - b) <= tolerance for a, b in zip(px, want))


def shows_colour(sent, tolerance=0):
    return len(sent) == 1 and all(near(px, COLOUR + (255,), tolerance) for px in pixels(sent[0]))


def png(width, height, rgba):
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\0" + bytes(rgba) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def magick_cases(aglow, root, skipped):
    magick = shutil.which("magick")
    if not magick:
        skipped.append("ImageMagick (magick is not installed)")
        return
    colour = "rgb(%d,%d,%d)" % COLOUR
    made = {}
    # The format, its file and how magick writes it; lossy formats within a tolerance.
    for name, args, tolerance in (("in.avif", ["avif:"], 6), ("in.heic", ["heic:"], 6), ("in.jxl", ["jxl:"], 3),
                                  ("in.qoi", ["qoi:"], 0), ("in.tga", ["tga:"], 0), ("in.ico", ["ico:"], 0),
                                  ("in.cur", ["cur:"], 0), ("rle.bmp", ["-type", "palette", "-compress", "RLE", "bmp3:"], 0)):
        target = root / name
        options, coder = args[:-1], args[-1]
        result = subprocess.run([magick, "-size", "3x2", "xc:" + colour, *options, coder + str(target)], capture_output=True)
        if result.returncode != 0 or not target.exists():
            skipped.append(f"{name} (magick cannot write it)")
            continue
        made[name] = target
        code, sent, err = run(aglow, target)
        check(code == 0 and shows_colour(sent, tolerance), name, code, err, sent and pixels(sent[0])[:2])
    if "rle.bmp" in made:
        data = made["rle.bmp"].read_bytes()
        check(struct.unpack("<I", data[30:34])[0] == 1, "the BMP is not run-length encoded")
    svg = root / "in.svg"
    svg.write_text('<?xml version="1.0"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="3" height="2">'
                   '<rect width="3" height="2" fill="%s"/></svg>' % colour)
    code, sent, err = run(aglow, svg)
    check(code == 0 and shows_colour(sent), "svg", code, err, sent and pixels(sent[0])[:2])
    print("each format through ImageMagick: passed", flush=True)

    # aglow gives magick the data, never the name: names that ImageMagick reads as a coder or a file
    # list show the TGA that they hold.
    if "in.tga" in made:
        for name in ("label:evil.tga", "@list.tga", "|echo.tga"):
            target = root / name
            shutil.copyfile(made["in.tga"], target)
            code, sent, err = run(aglow, target)
            check(code == 0 and shows_colour(sent), name, code, err)
    # MVG, a drawing language that ImageMagick reads, is no image to aglow.
    mvg = root / "draw.mvg"
    mvg.write_text("push graphic-context\nviewbox 0 0 3 2\nfill red\nrectangle 0 0 3 2\npop graphic-context\n")
    code, sent, err = run(aglow, mvg)
    check(code == 1 and not sent and "not an image that aglow can read" in err, code, err)
    # An SVG that names an image file of this computer: the file does not show.
    local = root / "secret.png"
    local.write_bytes(png(4, 4, (255, 0, 0, 255)))
    linked = root / "linked.svg"
    linked.write_text('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="4" '
                      'height="4"><image width="4" height="4" xlink:href="%s"/></svg>' % local.as_uri())
    code, sent, err = run(aglow, linked)
    check(code == 0 and len(sent) == 1 and all(px[3] == 0 for px in pixels(sent[0])), code, err,
          sent and pixels(sent[0])[:2])
    print("names and linked files do not reach ImageMagick: passed", flush=True)

    target = made.get("in.qoi", svg)
    code, sent, err = run(aglow, target, "--engine=builtin")
    check(code == 1 and not sent and "--engine builtin" in err, code, err)
    code, sent, err = run(aglow, target, env=environment(PATH=str(root / "nothing")))
    check(code == 1 and not sent and "magick is not installed" in err, code, err)
    # Each run of magick has a directory of its own for its policy and its files, which goes after it.
    private = root / "tmp"
    private.mkdir()
    code, sent, err = run(aglow, target, env=environment(TMPDIR=str(private)))
    check(code == 0 and not any(private.iterdir()), code, err, list(private.iterdir()))
    print("--engine builtin, no magick, and no files left: passed", flush=True)

    # A directory: its SVG shows with ImageMagick, and is no image with --engine builtin.
    folder = root / "folder"
    folder.mkdir()
    (folder / "a.png").write_bytes(png(2, 1, COLOUR + (255,)))
    shutil.copyfile(svg, folder / "b.svg")
    code, sent, err = run(aglow, folder)
    check(code == 0 and len(sent) == 2 and all(shows_colour([image]) for image in sent), code, err, len(sent))
    code, sent, err = run(aglow, folder, "--engine=builtin")
    check(code == 0 and len(sent) == 1, code, err, len(sent))
    print("directories: passed", flush=True)


class Server(http.server.BaseHTTPRequestHandler):
    image = png(2, 1, COLOUR + (255,))

    def do_GET(self):
        if self.path == "/image.png":
            self.send_response(200)
            self.send_header("Content-Length", str(len(self.image)))
            self.end_headers()
            self.wfile.write(self.image)
        elif self.path == "/moved":
            self.send_response(302)
            self.send_header("Location", "/image.png")
            self.end_headers()
        elif self.path == "/to-file":
            self.send_response(302)
            self.send_header("Location", "file:///etc/hostname")
            self.end_headers()
        elif self.path == "/huge":
            # A length past the limit: curl stops before the data.
            self.send_response(200)
            self.send_header("Content-Length", str(4 << 30))
            self.end_headers()
        else:
            self.send_error(404)

    def log_message(self, *args):
        pass


def curl_cases(aglow, root, skipped):
    if not shutil.which("curl"):
        skipped.append("URLs (curl is not installed)")
        return
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Server)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = "http://127.0.0.1:%d" % server.server_address[1]
    try:
        for path in ("/image.png", "/moved"):
            code, sent, err = run(aglow, base + path)
            check(code == 0 and shows_colour(sent), path, code, err)
        code, sent, err = run(aglow, base + "/to-file")
        check(code == 1 and not sent and "Could not download" in err and "file" in err.lower(), code, err)
        code, sent, err = run(aglow, base + "/huge")
        check(code == 1 and not sent and "larger than aglow reads" in err, code, err)
        code, sent, err = run(aglow, base + "/missing")
        check(code == 1 and not sent and "404" in err, code, err)
        code, sent, err = run(aglow, base + "/image.png", env=environment(PATH=str(root / "nothing")))
        check(code == 1 and not sent and "curl is not installed" in err, code, err)
    finally:
        server.shutdown()
        server.server_close()
    print("URLs through curl: passed", flush=True)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: icat_external.py AGLOW")
    aglow = str(Path(sys.argv[1]).resolve())
    skipped = []
    with tempfile.TemporaryDirectory(prefix="aglow-icat-external-") as temporary:
        root = Path(temporary)
        magick_cases(aglow, root, skipped)
        curl_cases(aglow, root, skipped)
    if skipped:
        print("skipped:", ", ".join(skipped), flush=True)
        sys.exit(SKIP)


if __name__ == "__main__":
    main()
