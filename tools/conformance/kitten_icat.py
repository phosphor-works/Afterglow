#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
"""kitten icat, the image client of kitty, against the terminal core of Afterglow.
Usage: kitten_icat.py AFTERGLOW_HEADLESS [AFTERGLOW_RENDERPNG]
kitten runs in afterglow_headless, which answers its queries, and the
images, placements and placeholder cells that reach the terminal are read
from the report of --graphics. It checks:
- each transfer mode (detect, stream, file and memory), with a PNG that
  kitten sends as it is and with one that it converts (mirrored), and that
  the terminal deletes every temporary file and shared memory object;
- --place, Unicode placeholders and the frames of an animated GIF;
- kitten in a private tmux, which passes the commands through to the
  terminal and shows the placeholders (skipped without tmux 3.3 or later).
With AFTERGLOW_RENDERPNG, what kitten writes with --use-window-size is also
drawn: the image in the middle of the line, and Unicode placeholders. That
part needs Vulkan, and so a Wayland display: without one it is skipped.
Exit status 77: kitten is not installed.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# The image helpers of the aglow icat test: python -I leaves the script's
# own directory out of the path. The import writes no __pycache__.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "aglow" / "tests"))
from icat_render import BLUE, GREEN, RED, check, gif, near, png, read_png  # noqa: E402

SKIP = 77
COLUMNS, ROWS = 40, 12
# The cell size of afterglow_headless (PtyHost::CellWidth and CellHeight).
CELL_WIDTH, CELL_HEIGHT = 10, 20
# An image of 8x2 cells: red on the left, blue on the right, a green corner.
WIDTH, HEIGHT = 8 * CELL_WIDTH, 2 * CELL_HEIGHT


def hex_colour(colour):
    return "%02x%02x%02xff" % colour


def parse_report(text):
    """The lines of afterglow_headless --graphics, as images, placements and a placeholder count."""
    images, placements, placeholders = {}, [], None
    for line in text.splitlines():
        words = line.split()
        if words[0] == "image":
            images[words[1]] = {"size": words[2], "frames": [], "animation": int(words[4].split("=")[1])}
        elif words[0] == "frame":
            fields = dict(word.split("=") for word in words[4:])
            images[words[1]]["frames"].append(dict(fields, size=words[3]))
        elif words[0] == "placement":
            placements.append(dict(word.split("=") for word in words[2:]))
        elif words[0] == "placeholders":
            placeholders = int(words[1])
    check(placeholders is not None, "no report", text)
    return images, placements, placeholders


def shm_objects():
    try:
        return {name for name in os.listdir("/dev/shm") if name.startswith("icat-")}
    except FileNotFoundError:
        return set()


class Live:
    def __init__(self, headless, root):
        self.headless = headless
        self.root = root
        self.temporary = root / "tmp"
        self.temporary.mkdir()
        self.environment = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(root),
                            "TMPDIR": str(self.temporary), "LANG": "C.UTF-8"}

    def run(self, *arguments):
        """kitten icat ARGUMENTS in the terminal core. The report of what it showed."""
        report = self.root / "graphics.txt"
        report.unlink(missing_ok=True)
        before = shm_objects()
        result = subprocess.run([self.headless, "--cols", str(COLUMNS), "--rows", str(ROWS), "--dump", "--graphics", str(report),
                                 "--", "kitten", "icat", *arguments], cwd=self.root, env=self.environment,
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        check(result.returncode == 0, arguments, result.returncode, result.stderr.decode(errors="replace"))
        # The terminal deletes what kitten made for it to read.
        left = shm_objects() - before
        check(not left, arguments, "shared memory left behind", left)
        check(not any(self.temporary.iterdir()), arguments, "files left behind", list(self.temporary.iterdir()))
        return parse_report(report.read_text())


def only_image(images):
    check(len(images) == 1, "one image", images)
    return next(iter(images.values()))


def check_still(images, mirrored):
    image = only_image(images)
    check(image["size"] == f"{WIDTH}x{HEIGHT}", image)
    frame = image["frames"][0]
    # The centre pixel is the first of the right half.
    want = (BLUE, RED, RED) if mirrored else (GREEN, BLUE, BLUE)
    check((frame["topleft"], frame["centre"], frame["bottomright"]) == tuple(map(hex_colour, want)), frame)


def live_checks(headless, root):
    live = Live(headless, root)
    (root / "still.png").write_bytes(png(WIDTH, HEIGHT, lambda x, y: (
        GREEN if x < CELL_WIDTH and y < CELL_WIDTH else RED if x < WIDTH // 2 else BLUE)))
    for mode in ("detect", "stream", "file", "memory"):
        for mirrored in (False, True):
            # The options go first: kitten takes every argument after a file as a file.
            arguments = [f"--transfer-mode={mode}", *(["--mirror=horizontal"] if mirrored else []), "still.png"]
            images, placements, _ = live.run(*arguments)
            check_still(images, mirrored)
            # Centred: 16 columns before the 8 of the image.
            check(len(placements) == 1 and placements[0]["col"] == "16" and placements[0]["virtual"] == "0", placements)
        print(f"--transfer-mode={mode}, as it is and converted: passed", flush=True)

    # kitten scales the pixels into the 4x2 cells itself, with the image's
    # aspect: 4 cells wide, 1 high. It moves the cursor to the place.
    images, placements, _ = live.run("--place=4x2@0x1", "--align=left", "still.png")
    image = only_image(images)
    check(image["size"] == f"{4 * CELL_WIDTH}x{CELL_HEIGHT}", image)
    check(image["frames"][0]["topleft"] == hex_colour(GREEN) and image["frames"][0]["bottomright"] == hex_colour(BLUE), image)
    check(len(placements) == 1 and placements[0]["col"] == "0", placements)
    print("--place: passed", flush=True)

    images, placements, placeholders = live.run("--unicode-placeholder", "still.png")
    check_still(images, False)
    check(len(placements) == 1 and placements[0]["virtual"] == "1", placements)
    check(placeholders == 8 * 2, "a placeholder cell for each cell of the image", placeholders)
    print("Unicode placeholders: passed", flush=True)

    (root / "anim.gif").write_bytes(gif(WIDTH, HEIGHT, [RED, GREEN, BLUE]))
    images, placements, _ = live.run("anim.gif")
    image = only_image(images)
    centres = [frame["centre"] for frame in image["frames"]]
    check(centres == [hex_colour(c) for c in (RED, GREEN, BLUE)], image)
    check(all(frame["gap"] == "200" for frame in image["frames"]), "0.2 s for each frame", image)
    check(image["animation"] == 2, "the animation runs", image)
    print("animated GIF: passed", flush=True)


def tmux_check(headless, root):
    """kitten icat in a private tmux: the commands pass through it, and the placeholders show."""
    if not shutil.which("tmux"):
        print("kitten icat in tmux: skipped: tmux is not installed", flush=True)
        return
    version = subprocess.run(["tmux", "-V"], capture_output=True, text=True).stdout
    found = re.search(r"(\d+)\.(\d+)", version)
    if not found or (int(found.group(1)), int(found.group(2))) < (3, 3):
        print(f"kitten icat in tmux: skipped: {version.strip()} has no allow-passthrough", flush=True)
        return
    sockets = root / "tmux"
    sockets.mkdir(mode=0o700)
    temporary = root / "tmux-tmp"
    temporary.mkdir()
    environment = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(root), "TMPDIR": str(temporary),
                   "LANG": "C.UTF-8", "TMUX_TMPDIR": str(sockets)}
    tmux = ["tmux", "-L", "kitten-icat", "-f", "/dev/null"]
    report = root / "tmux-graphics.txt"
    done = root / "tmux-done"
    # The pane stays after kitten, so that the report shows its screen.
    script = f"kitten icat still.png; echo $? >{done}; exec sleep 60"
    process = subprocess.Popen([headless, "--cols", str(COLUMNS), "--rows", str(ROWS), "--graphics", str(report), "--",
                                *tmux, "new-session", "-x", str(COLUMNS), "-y", str(ROWS), "sh", "-c", script],
                               cwd=root, env=environment, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 30
        shown = None
        while time.monotonic() < deadline and process.poll() is None:
            if done.exists() and report.exists():
                images, placements, placeholders = parse_report(report.read_text())
                if images and placeholders:
                    shown = (images, placements, placeholders)
                    break
            time.sleep(0.1)
        check(done.exists() and done.read_text().strip() == "0", "kitten in tmux failed",
              done.read_text() if done.exists() else "no status")
        check(shown is not None, "no image and placeholders reached the terminal",
              report.read_text() if report.exists() else "no report")
        images, placements, placeholders = shown
        check_still(images, False)
        check(len(placements) == 1 and placements[0]["virtual"] == "1", placements)
        check(placeholders == 8 * 2, placeholders)
    finally:
        subprocess.run([*tmux, "kill-server"], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=30)
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    print("kitten icat in tmux: passed", flush=True)


def render_checks(renderer, root):
    """What kitten writes for a window of COLUMNS x ROWS, drawn by afterglow_renderpng."""
    if not os.environ.get("WAYLAND_DISPLAY"):
        print("drawn images: skipped: afterglow_renderpng needs a Wayland display for Vulkan", flush=True)
        return
    render_environment = {key: value for key, value in os.environ.items() if key != "TMUX"}
    render_environment.update(QT_QPA_PLATFORM="wayland", QT_QPA_PLATFORMTHEME="generic")

    def render(name, stream):
        (root / (name + ".vt")).write_bytes(stream)
        screen = root / (name + "-screen.png")
        result = subprocess.run([renderer, "--cols", str(COLUMNS), "--rows", str(ROWS), str(root / (name + ".vt")),
                                 str(screen)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                env=render_environment, timeout=120)
        if result.returncode != 0 and b"\nno Vulkan: " in b"\n" + result.stderr:
            return None
        check(result.returncode == 0, name, result.returncode, result.stderr.decode(errors="replace"))
        return read_png(screen)

    blank = render("blank", b"")
    if blank is None:
        print("drawn images: skipped: no Vulkan", flush=True)
        return
    cell_width, cell_height = len(blank[0]) // COLUMNS, len(blank) // ROWS
    (root / "drawn.png").write_bytes(png(8 * cell_width, 4 * cell_height, lambda x, y: (
        GREEN if x < cell_width and y < cell_height else RED if x < 4 * cell_width else BLUE)))
    environment = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(root), "LANG": "C.UTF-8"}

    def icat(*arguments):
        # No terminal to ask: the window's size is given, and the data goes in the stream.
        result = subprocess.run(["kitten", "icat", "--use-window-size",
                                 f"{COLUMNS},{ROWS},{COLUMNS * cell_width},{ROWS * cell_height}", "--transfer-mode=stream",
                                 "--stdin=no", *arguments, str(root / "drawn.png")], env=environment,
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60,
                                start_new_session=True)
        check(result.returncode == 0, arguments, result.stderr.decode(errors="replace"))
        return result.stdout

    left = 16 * cell_width
    for name, extra, after in (("centred", (), b""), ("placeholder", ("--unicode-placeholder",), b"\x1b[%d;1H" % ROWS)):
        # A placeholder image is drawn under the cursor, as kitty draws it: the cursor moves away.
        pixels = render(name, icat(*extra) + after)
        check(near(pixels[cell_height // 2][left + cell_width // 2], GREEN), name, pixels[cell_height // 2][left + cell_width // 2])
        check(near(pixels[3 * cell_height][left + 2 * cell_width], RED), name)
        check(near(pixels[3 * cell_height][left + 6 * cell_width], BLUE), name)
        check(not near(pixels[3 * cell_height][left - cell_width], RED), name, "nothing left of the image")
    print("drawn images, placed and with placeholders: passed", flush=True)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit("usage: kitten_icat.py AFTERGLOW_HEADLESS [AFTERGLOW_RENDERPNG]")
    if not shutil.which("kitten"):
        print("skipped: kitten is not installed")
        sys.exit(SKIP)
    # The programs run in the test's directory.
    headless, *renderer = (os.path.abspath(path) for path in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="afterglow-kitten-icat-") as temporary:
        root = Path(temporary)
        live_checks(headless, root)
        tmux_check(headless, root)
        if renderer:
            render_checks(renderer[0], root)


if __name__ == "__main__":
    main()
