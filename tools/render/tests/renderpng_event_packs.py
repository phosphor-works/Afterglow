#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""The bundled event packs in afterglow_renderpng: they load, and they start and end on the window.
Usage: renderpng_event_packs.py AFTERGLOW_RENDERPNG
The Phosphor and plain packs of Shaders.overview, tabTransition and paneEffects draw
only while their event shows. At the ends of an event the window must show
as it does with no effect, pixel for pixel, or the picture jumps when the
pack stops. In the middle of an event the picture must differ, or the pack draws
nothing. afterglow_renderpng draws with Vulkan, which needs a Wayland
display: without a display or a Vulkan device the test exits with 77, which
CTest reports as skipped. It runs on the wayland platform with the generic
platform theme, so that the desktop's theme plugin is not loaded.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

COLUMNS, ROWS = 40, 12
SKIP = 77
# Two panes with text and colours, so a pack that moves or dims a pane shows.
STREAM = (b"\x1b[?25l\x1b[41m red \x1b[0m plain text \x1b[32mgreen\x1b[0m\r\n" + b"0123456789 abcdefghij\r\n" * 6
          + b"\x1b[44m blue \x1b[0m end")


def check(condition, *detail):
    """Raises AssertionError when condition is false. Not assert: python -O must not turn the checks off."""
    if not condition:
        raise AssertionError(*detail)


def pixels(path):
    """The size and the unfiltered rows of an 8-bit RGB or RGBA PNG."""
    data = Path(path).read_bytes()
    check(data.startswith(b"\x89PNG"), path)
    at, idat = 8, b""
    width = height = colour = None
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            check(depth == 8 and colour in (2, 6), "unexpected PNG format", depth, colour)
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    check(width is not None, "no IHDR chunk", path)
    channels = 4 if colour == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    previous = bytearray(stride)
    out = bytearray()
    for row in range(height):
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
        out += line
    return (width, height, channels, bytes(out))


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: renderpng_event_packs.py AFTERGLOW_RENDERPNG")
    renderer = sys.argv[1]
    if not os.environ.get("WAYLAND_DISPLAY"):
        print("skipped: afterglow_renderpng needs a Wayland display for Vulkan, and WAYLAND_DISPLAY is not set")
        sys.exit(SKIP)
    with tempfile.TemporaryDirectory(prefix="afterglow-renderpng-") as temporary:
        root = Path(temporary)
        # Not the developer's packs: a build-tree binary then reads the checkout's alone.
        environment = dict(os.environ, QT_QPA_PLATFORM="wayland", QT_QPA_PLATFORMTHEME="generic",
                           XDG_CONFIG_HOME=str(root / "config"), XDG_DATA_HOME=str(root / "data"))
        (root / "stream.vt").write_bytes(STREAM)
        # A pack that draws its input as it is: what the window shows with an
        # event's state and no effect. A pane that closes is not in the window
        # of afterglow_renderpng, so the picture with no pack is not that.
        through = root / "config/afterglow/shaders/through"
        through.mkdir(parents=True)
        (through / "metadata.json").write_text('{"id": "through", "name": "Through", "fragmentShader": "effect.frag"}')
        (through / "effect.frag").write_text("vec4 pTerminal(vec2 uv) { return terminalTexel(uv); }\n")
        count = 0

        def run(*options):
            nonlocal count
            count += 1
            target = root / f"{count}.png"
            result = subprocess.run([renderer, "--cols", str(COLUMNS), "--rows", str(ROWS), "--split", *options,
                                     str(root / "stream.vt"), str(target)], stdout=subprocess.DEVNULL,
                                    stderr=subprocess.PIPE, env=environment, timeout=120)
            return result, target

        def render(*options):
            result, target = run(*options)
            check(result.returncode == 0, options, result.stderr.decode(errors="replace"))
            return pixels(target)

        # The first render tells a missing Vulkan from a failure of the test.
        probe, target = run()
        if probe.returncode != 0 and b"\nno Vulkan: " in b"\n" + probe.stderr:
            print("skipped:", probe.stderr.decode(errors="replace").strip())
            sys.exit(SKIP)
        check(probe.returncode == 0, probe.stderr.decode(errors="replace"))
        plain = pixels(target)

        def same(name, *options):
            check(render(*options) == plain, name, "must show the window as it is", options)

        def differs(name, *options):
            check(render(*options) != plain, name, "must draw", options)

        def rests(name, kind, pack, *options):
            check(render(kind, pack, *options) == render(kind, "through", *options), name,
                  "must show the window as it is", options)

        def draws(name, kind, pack, *options):
            check(render(kind, pack, *options) != render(kind, "through", *options), name, "must draw", options)

        # The Phosphor packs and the plain packs share their motion: both
        # families get the same checks.
        for family in ("phosphor", "plain"):
            tabs = ("--tab-transition", family + "-tabs")
            for style in ("0", "1"):
                chosen = (*tabs, "--param", family + "-tabs:style=" + style)
                same("a tab switch that has arrived", *chosen, "--transition-progress", "1")
                for direction in ("1", "-1"):
                    differs("a tab switch on its way", *chosen, "--transition-progress", "0.5",
                            "--transition-direction", direction)
            # Both styles start on the tab being left, whichever way the switch goes.
            left = render(*tabs, "--transition-progress", "0")
            check(left != plain, "the tab being left is another picture")
            check(render(*tabs, "--transition-progress", "0", "--transition-direction", "-1") == left,
                  "a slide to the left starts on the tab being left")
            check(render(*tabs, "--param", family + "-tabs:style=1", "--transition-progress", "0") == left,
                  "a fade starts on the tab being left")
            print(family + "-tabs: passed", flush=True)

            panes = ("--pane-effects", family + "-panes")
            same("a pane that has opened", *panes, "--open-progress", "1")
            rests("a pane that has opened", *panes, "--open-progress", "1")
            draws("a pane that opens", *panes, "--open-progress", "0.3")
            rests("a pane that has closed", *panes, "--close-progress", "1")
            draws("a pane that closes", *panes, "--close-progress", "0.2")
            back = ("--context", "0", "--previous-context", "1")
            same("a pane that has returned here", *panes, *back, "--context-progress", "1")
            draws("a pane that returns here", *panes, *back, "--context-progress", "0.3")
            # A pane that is away keeps its line, until the mark parameter removes it.
            for context in ("1", "2", "3"):
                draws("a pane that is away", *panes, "--context", context, "--context-progress", "1")
            rests("a pane that is away with no mark", *panes, "--context", "1", "--context-progress", "1", "--param",
                  family + "-panes:mark=0")
            for effect in ("1", "2", "3"):
                rests("a pulse that has ended", *panes, "--pulse", effect + ":1")
                draws("a pulse that runs", *panes, "--pulse", effect + ":0.3")
            print(family + "-panes: passed", flush=True)

            overview = ("--overview", family + "-overview")
            same("an overview that is closed", *overview, "--overview-amount", "0")
            rests("an overview of two tabs that is closed", *overview, "--overview-amount", "0", "--tabs", "2")
            draws("an overview that is open", *overview)
            draws("an overview that closes", *overview, "--overview-closing", "--overview-amount", "0.5")
            # Four cards. The highlight is on its way from card 0 to card 2: it
            # fades between those two, and does not light card 1 as it passes.
            there = (*overview, "--tabs", "2", "--highlight", "2")
            arrived = render(*there)
            check(render(*there, "--highlight-from", "0") == arrived,
                  "a highlight that has arrived shows nothing of the pane that it left")
            halfway = render(*there, "--highlight-from", "0", "--overview-position", "1")
            check(halfway != arrived, "a highlight on its way shows the pane that it left")
            check(halfway != render(*there, "--overview-position", "1"),
                  "a highlight on its way does not light the card between")
            print(family + "-overview: passed", flush=True)

        # The two families differ in their look, not only in their names.
        for options in (("--tab-transition", "{}-tabs", "--transition-progress", "0.5"),
                        ("--pane-effects", "{}-panes", "--open-progress", "0.3"), ("--overview", "{}-overview")):
            pictures = [render(*(o.format(family) for o in options)) for family in ("phosphor", "plain")]
            check(pictures[0] != pictures[1], "the Phosphor pack and the plain pack must differ", options)
        print("the two families differ: passed", flush=True)


if __name__ == "__main__":
    main()
