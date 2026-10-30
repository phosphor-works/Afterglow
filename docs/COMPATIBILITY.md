<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Terminal compatibility

Afterglow targets Linux/Wayland with Vulkan. The protocol implementation is in `libs/afterglow-vt`; renderer checks also inspect actual GPU pixels. These are feature-specific interoperability claims, not a claim of complete xterm, Kitty or iTerm2 equivalence.

## D1 protocol additions

| Feature | Supported behavior | Regression and interoperability fixture |
| --- | --- | --- |
| Kitty relative images | `P`, `Q`, signed `H`/`V`; parent movement; chained parents; cascade deletion; missing-parent, cycle and depth errors; virtual placeholder parents | `Graphics.Relative*` and transmit/placeholder pixel regressions; [moving-image client](../data/examples/compatibility/relative-images.py) |
| Kitty multiple cursors | Shapes 1/2/3/29, clear 0, coordinate types 0/2/4, color operations 30/40, capabilities and queries 100/101; clipping, reset, clear, alternate-screen lifecycle; blink and mixed shape rendering | `Extensions.MultipleCursors*`, `Extensions.ExtraCursorColors*`, `SnapshotCursor.*`, `Render.MultipleCursors*`; [multi-edit client](../data/examples/compatibility/multi-edit.py) |
| OSC 1337 downloads | `File`, `MultipartFile`, `FilePart`, `FileEnd`; explicit native Save dialog; cancellation; bounded data | `Extensions.Downloads*`, `desktop_acceptance.py`; [download client](../data/examples/compatibility/download.py), upstream [it2dl](https://iterm2.com/utilities/it2dl) |
| IME context | Live cursor-row surrounding text, before/after text, UTF-16 position and cursor rectangle | `DesktopText.*`; [IME behavior](DESKTOP.md#input-methods-and-directionality) |
| Native terminal accessibility | Viewport text, cursor, selection and character geometry through Qt/AT-SPI | `DesktopText.*`, `desktop_acceptance.py` |
| Local file drop | Local absolute paths quoted for Bash/Zsh/Fish, targeted pane, bracketed paste, no automatic Enter | `FileDrop.*`; [file-drop workflow](DESKTOP.md#drop-files-into-a-pane) |

Relative placement chains are limited to 32 parent links. Image metadata is bounded to 4,096 images, 4,096 placements and 1,024 frames per image; image data and pending transmissions have a 320 MiB ceiling, with the configured store quota controlling retained image data. An omitted parent placement ID resolves to the first matching placement when created. A virtual parent's origin is derived from its displayed Unicode placeholders, including implied diacritics; when none are displayed its children are hidden. Scroll/resize can change the anchor. Parent offsets are in cells, while `X`/`Y` remain the child's pixel offsets. A relative placement never moves the terminal cursor.

Multiple-cursor color operations 30 (text), 40 (fill) and query 101 are implemented. Color space 0 follows the main cursor, 1 reverses the relevant cell colors, 2 accepts RGB components and 5 uses a palette entry. Colors are shared by all extra cursors; they are resolved for each cell, including palette changes. Reverse fill takes the cell foreground and uses the cell background for text, overriding the explicit text-color choice. Capability queries return `1;2;3;29;30;40;100;101`; clients should inspect that response. CSI parsing accepts up to 4,096 numeric parameters per sequence and rejects an overflowing command as a whole; batch larger point sets into multiple sequences. Full reset restores default extra-cursor colors; clear/screen switches remove positions. Cursor locations are screen coordinates, do not scroll with output, disappear while viewing scrollback, and are clipped on resize. Mixed shapes use separate cursor layers so block text remains readable and beams remain above glyphs. With layer shaders enabled, the extra layer costs one additional RGBA8 frame texture (about 7.9 MiB at 1080p / 31.6 MiB at 4K); a second cursor shader pass runs only when opposite drawing orders are needed. Persistent cursor buffers then have independent history per cursor layer and share the existing allocation budget.

Downloads accept at most 8 MiB decoded data and one pending application request. They are not the Kitty transfer protocol, a resumable transfer service or automatic uploads. IME context does not provide arbitrary surrounding-text edits. Opt-in primary-screen bidi layout is available with explicit application-grid opt-outs and bounded paragraph processing; see [behavior and limits](BIDI.md).

Protocol definitions: [Kitty graphics](https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements), [Kitty multiple cursors](https://sw.kovidgoyal.net/kitty/multiple-cursors-protocol/), [iTerm2 file transport](https://iterm2.com/documentation-images.html).

## Existing coverage

The suite also covers terminal parsing and grid behavior, scrollback/reflow, Sixel, Kitty image transmission/placements/animation/Unicode placeholders, iTerm2 inline images, OSC 66 text sizing, keyboard and mouse encodings, shell markers, clipboard and notification extensions. Consult `libs/afterglow-vt/tests` for exact accepted sequences rather than assuming every optional operation of those protocols exists.

Run focused tests with `ctest --test-dir build --output-on-failure -R 'Graphics.Relative|Extensions.Multiple|Extensions.ExtraCursor|Extensions.Download|DesktopText|FileDrop|Render.Multiple|SnapshotCursor|Bidi'`. The opt-in native acceptance command is `python3 afterglow/tests/desktop_acceptance.py build/afterglow/afterglow`; it needs the documented nested-desktop dependencies, `python-dbus` and AT-SPI. It uses temporary config and a private session bus.
