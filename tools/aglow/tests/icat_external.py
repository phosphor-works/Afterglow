#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: LGPL-2.1-or-later
"""aglow icat with the programs that it runs: ImageMagick and curl.

Usage: icat_external.py AGLOW

ImageMagick: an image of each format that aglow sends to magick (AVIF, HEIC,
JPEG XL, QOI, SVG, TGA, ICO and CUR), made by magick here, and icons and BMPs
that hold a PNG or a JPEG, made here byte by byte. A run-length BMP decodes
without magick. A JPEG XL whose own orientation turns it shows upright. A
file whose name ImageMagick would read as a coder or a file list shows as
its data says. Data of a format outside the list (MVG) is refused without
magick. An SVG that names a local image shows without it. --engine builtin
and a PATH without magick refuse with a message. A magick that cannot run,
and one without the coder of SVG, say so. Each run of magick has a private
directory in TMPDIR, which goes after it. A directory of images shows its SVG
with magick, and not with --engine builtin.

curl: images from a local HTTP server that the test runs: a PNG, a redirect
to it, a redirect to a file: URL, which must fail, a file over the limit, a
missing file, a TGA whose URL alone says what it is, and a PATH without curl.

aglow and magick run with a private HOME, so that no ImageMagick
configuration of the user reaches them, and aglow in a session of its own,
with no controlling terminal.

It exits with 77 (skipped) after the other cases when magick or curl is not
installed, or magick cannot write a format, naming what it left out.
"""

import http.server
import os
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import icat_decode  # noqa: E402
from icat_decode import check, near, pixels, transmissions  # noqa: E402
from icat_decode import png_rgba as png  # noqa: E402

SKIP = 77
COLOUR = (200, 100, 50)
PROXIES = {"http_proxy", "https_proxy", "ftp_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "FTP_PROXY", "ALL_PROXY"}
# Seconds that magick gets to make a test image.
MAKE_TIMEOUT = 60


def environment(**extra):
    """The environment of the test (with its private HOME), without TMUX, proxies and ImageMagick's own variables."""
    env = {key: value for key, value in os.environ.items()
           if key != "TMUX" and key not in PROXIES and not key.startswith("MAGICK_")}
    env.update(extra)
    return env


def run(aglow, target, *options, env=None):
    """aglow icat on TARGET, a path or a URL, decoded to pixels (--mirror makes it decode a PNG too)."""
    code, out, err = icat_decode.run(aglow, target, "--mirror=horizontal", *options, env=env or environment(), timeout=120)
    return code, transmissions(out), err


def shows_colour(sent, tolerance=0):
    return len(sent) == 1 and all(near(px, COLOUR + (255,), tolerance) for px in pixels(sent[0]))


def make(magick, *args):
    """Runs magick with ARGS to make a test image: whether it made it."""
    try:
        return subprocess.run([magick, *args], capture_output=True, env=environment(), timeout=MAKE_TIMEOUT).returncode == 0
    except subprocess.TimeoutExpired:
        return False


def icon_of(image):
    """An ICO of one entry, 3 by 2, whose data is IMAGE (a PNG)."""
    return struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", 3, 2, 0, 0, 1, 32, len(image), 22) + image


def bmp_of(image, compression):
    """A BMP, 3 by 2, whose pixels are IMAGE: a JPEG (compression 4) or a PNG (compression 5)."""
    header = struct.pack("<IiiHHIIiiII", 40, 3, 2, 1, 0, compression, len(image), 2835, 2835, 0, 0)
    return b"BM" + struct.pack("<IHHI", 14 + len(header) + len(image), 0, 0, 14 + len(header)) + header + image


def program(path, text):
    """Writes the script TEXT at PATH, executable."""
    path.write_text(text)
    path.chmod(0o755)


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
                                  ("in.cur", ["cur:"], 0), ("in.jpg", ["-quality", "100", "jpg:"], 6),
                                  ("rle.bmp", ["-type", "palette", "-compress", "RLE", "bmp3:"], 0)):
        target = root / name
        options, coder = args[:-1], args[-1]
        if not make(magick, "-size", "3x2", "xc:" + colour, *options, coder + str(target)) or not target.exists():
            skipped.append(f"{name} (magick cannot write it)")
            continue
        made[name] = target
        if name in ("in.jpg", "rle.bmp"):
            continue
        code, sent, err = run(aglow, target)
        check(code == 0 and shows_colour(sent, tolerance), name, code, err, sent and pixels(sent[0])[:2])
    # A run-length BMP of 8 bits decodes without ImageMagick.
    if "rle.bmp" in made:
        data = made["rle.bmp"].read_bytes()
        check(struct.unpack("<I", data[30:34])[0] == 1 and struct.unpack("<H", data[28:30])[0] == 8,
              "the BMP is not run-length encoded at 8 bits")
        code, sent, err = run(aglow, made["rle.bmp"], "--engine=builtin")
        check(code == 0 and shows_colour(sent), "rle.bmp", code, err, sent and pixels(sent[0])[:2])
    # An icon whose image is a PNG, and BMPs that hold a PNG or a JPEG: ImageMagick reads the image inside
    # with the coder of its format, which aglow's policy allows for these.
    inner = png(3, 2, COLOUR + (255,))
    held = [("png.ico", icon_of(inner), 0), ("png.bmp", bmp_of(inner, 5), 0)]
    if "in.jpg" in made:
        held.append(("jpeg.bmp", bmp_of(made["in.jpg"].read_bytes(), 4), 6))
    for name, data, tolerance in held:
        (root / name).write_bytes(data)
        code, sent, err = run(aglow, root / name)
        check(code == 0 and shows_colour(sent, tolerance), name, code, err, sent and pixels(sent[0])[:2])
    # A JPEG XL whose orientation is its own (no EXIF): red then blue, turned a quarter clockwise, shows
    # 1 wide and 2 high, red at the top.
    turned = root / "turned.jxl"
    if make(magick, "-size", "1x1", "xc:red", "-size", "1x1", "xc:blue", "+append", "-orient", "RightTop",
            "-quality", "100", "jxl:" + str(turned)) and turned.exists():
        code, sent, err = run(aglow, turned)
        check(code == 0 and len(sent) == 1 and sent[0]["keys"].get("s") == "1" and sent[0]["keys"].get("v") == "2",
              code, err, sent and sent[0]["keys"])
        got = pixels(sent[0])
        check(near(got[0], (255, 0, 0, 255), 3) and near(got[1], (0, 0, 255, 255), 3), got)
    else:
        skipped.append("a turned JPEG XL (magick cannot write it)")
    svg = root / "in.svg"
    svg.write_text('<?xml version="1.0"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="3" height="2">'
                   '<rect width="3" height="2" fill="%s"/></svg>' % colour)
    code, sent, err = run(aglow, svg)
    check(code == 0 and shows_colour(sent), "svg", code, err, sent and pixels(sent[0])[:2])
    # aglow started without standard input: the pipe of magick's standard input takes descriptor 0 at
    # first, and must still be magick's standard input after exec().
    result = subprocess.run([aglow, *icat_decode.ICAT, "--mirror=horizontal", str(svg)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=environment(), timeout=120, start_new_session=True,
                            preexec_fn=lambda: os.close(0))
    check(result.returncode == 0 and shows_colour(transmissions(result.stdout)), result.returncode, result.stderr)
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
    # Each run of magick has a directory of its own for its policy and its files, in TMPDIR, which goes
    # after it. A magick on PATH before the real one says which directory it got.
    private = root / "tmp"
    private.mkdir()
    wrapper = root / "wrapper"
    wrapper.mkdir()
    log = root / "configure-path"
    program(wrapper / "magick", f'#!/bin/sh\nprintf "%s" "$MAGICK_CONFIGURE_PATH" > {shlex.quote(str(log))}\n'
                                f'exec {shlex.quote(magick)} "$@"\n')
    code, sent, err = run(aglow, target, env=environment(TMPDIR=str(private), PATH=f"{wrapper}:{os.environ.get('PATH', '')}"))
    used = log.read_text() if log.exists() else ""
    check(code == 0 and used.startswith(str(private) + "/aglow-magick-") and not Path(used).exists(), code, err, used)
    check(not any(private.iterdir()), list(private.iterdir()))
    print("--engine builtin, no magick, and no files left: passed", flush=True)

    # A magick that cannot run, and one without the coder of SVG (librsvg): each says what is wrong.
    fake = root / "fake"
    fake.mkdir()
    (fake / "magick").write_bytes(b"\0 not a program\n")
    (fake / "magick").chmod(0o755)
    code, sent, err = run(aglow, svg, env=environment(PATH=str(fake)))
    check(code == 1 and not sent and "cannot run magick: Exec format error" in err, code, err)
    program(fake / "magick", "#!/bin/sh\necho \"magick: no decode delegate for this image format \\`RSVG:-' "
                             "@ error/constitute.c/ReadImageInternal/739.\" >&2\nexit 1\n")
    code, sent, err = run(aglow, svg, env=environment(PATH=str(fake)))
    check(code == 1 and not sent and "librsvg coder (RSVG), which this ImageMagick does not have" in err, code, err)
    print("a magick that cannot run, or cannot read SVG: passed", flush=True)

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
    # A TGA without the footer that names it: only its name says what it is.
    tga = b""

    def do_GET(self):
        if self.path in ("/image.png", "/picture.tga?size=1", "/picture%2Etga", "/picture?size=1"):
            data = self.image if self.path == "/image.png" else self.tga
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        elif self.path == "/moved":
            self.send_response(302)
            self.send_header("Location", "/image.png")
            self.end_headers()
        elif self.path == "/elsewhere":
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
        # curl names the protocol that it refused ("file"), which the URL does not hold.
        code, sent, err = run(aglow, base + "/elsewhere")
        check(code == 1 and not sent and "Could not download" in err and '"file"' in err, code, err)
        code, sent, err = run(aglow, base + "/huge")
        check(code == 1 and not sent and "larger than aglow reads" in err, code, err)
        code, sent, err = run(aglow, base + "/missing")
        check(code == 1 and not sent and "404" in err, code, err)
        code, sent, err = run(aglow, base + "/image.png", env=environment(PATH=str(root / "nothing")))
        check(code == 1 and not sent and "curl is not installed" in err, code, err)
        # The name of the URL's path tells a TGA, without its query: the same data at a URL without the name
        # is no image.
        tga = root / "in.tga"
        if tga.exists():
            Server.tga = tga.read_bytes()
            code, sent, err = run(aglow, base + "/picture.tga?size=1")
            check(code == 0 and shows_colour(sent), code, err)
            # The name with its escapes decoded.
            code, sent, err = run(aglow, base + "/picture%2Etga")
            check(code == 0 and shows_colour(sent), code, err)
            code, sent, err = run(aglow, base + "/picture?size=1")
            check(code == 1 and not sent and "not an image that aglow can read" in err, code, err)
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
        # No ImageMagick configuration of the user (~/.config/ImageMagick, ~/.magick) reaches magick.
        home = root / "home"
        home.mkdir()
        os.environ.update(HOME=str(home), XDG_CONFIG_HOME=str(home / ".config"))
        magick_cases(aglow, root, skipped)
        curl_cases(aglow, root, skipped)
    if skipped:
        print("skipped:", ", ".join(skipped), flush=True)
        sys.exit(SKIP)


if __name__ == "__main__":
    main()
