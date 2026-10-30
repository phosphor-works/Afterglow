<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Terminal compatibility

Afterglow targets Linux and Wayland with Vulkan.
The protocol implementation is in `libs/afterglow-vt`. Renderer tests also check GPU pixels.
The following table describes tested features. It does not claim complete xterm, Kitty or iTerm2 compatibility.

## Supported protocol features

| Feature | Supported behavior | Regression and interoperability fixture |
| --- | --- | --- |
| Kitty relative images | `P`, `Q`, signed `H`/`V`; parent movement; chained parents; cascade deletion; missing-parent, cycle and depth errors; virtual placeholder parents | `Graphics.Relative*` and transmit/placeholder pixel regressions; [moving-image client](../data/examples/compatibility/relative-images.py) |
| Kitty multiple cursors | Shapes 1/2/3/29, clear 0, coordinate types 0/2/4, color operations 30/40, capabilities and queries 100/101; clipping, reset, clear, alternate-screen lifecycle; blink and mixed shape rendering | `Extensions.MultipleCursors*`, `Extensions.ExtraCursorColors*`, `SnapshotCursor.*`, `Render.MultipleCursors*`; [multi-edit client](../data/examples/compatibility/multi-edit.py) |
| OSC 1337 downloads | `File`, `MultipartFile`, `FilePart`, `FileEnd`; explicit native Save dialog; cancellation; bounded data | `Extensions.Downloads*`, `desktop_acceptance.py`; [download client](../data/examples/compatibility/download.py), upstream [it2dl](https://iterm2.com/utilities/it2dl) |
| IME context | Live cursor-row surrounding text, before/after text, UTF-16 position and cursor rectangle | `DesktopText.*`; [IME behavior](DESKTOP.md#input-methods-and-directionality) |
| Native terminal accessibility | Viewport text, cursor, selection and character geometry through Qt/AT-SPI | `DesktopText.*`, `desktop_acceptance.py` |
| Local file drop | Local absolute paths quoted for Bash/Zsh/Fish, targeted pane, bracketed paste, no automatic Enter | `FileDrop.*`; [file-drop workflow](DESKTOP.md#drop-files-into-a-pane) |


## Relative images

Relative placement chains permit 32 parent links.
Metadata limits are 4,096 images, 4,096 placements and 1,024 frames per image.
Image data and pending transmissions have a 320 MiB limit.
The configured store quota controls retained image data.

An omitted parent placement ID selects the first matching placement at creation.
Displayed Unicode placeholders determine a virtual parent's origin, including implied diacritics.
Children are hidden when no parent placeholders are displayed.
Scrolling or resizing can change the anchor.
Parent offsets use cells. `X` and `Y` remain the child's pixel offsets.
Relative placements do not move the terminal cursor.

## Multiple cursors

Color operations 30 (text), 40 (fill) and query 101 are supported.

| Color space | Behavior |
| --- | --- |
| 0 | Follow the main cursor. |
| 1 | Reverse the relevant cell colors. |
| 2 | Use RGB components. |
| 5 | Use a palette entry. |

All extra cursors share color settings. Each cell resolves those settings, including palette changes.
Reverse fill uses the cell foreground for fill and the cell background for text.
It overrides the explicit text-color choice.
Capability queries return `1;2;3;29;30;40;100;101`. Clients must inspect the response.

CSI parsing permits 4,096 numeric parameters per sequence.
An overflow rejects the complete command. Divide larger point sets across sequences.
Full reset restores default colors. Clear and screen switches remove positions.
Positions use screen coordinates and do not scroll with output.
Cursors disappear while viewing scrollback. Resize clips positions to the screen.

Separate layers keep block text readable and beams above glyphs.
With layer shaders, mixed cursor order requires one additional RGBA8 frame texture.
This costs approximately 7.9 MiB at 1080p or 31.6 MiB at 4K.
A second cursor shader pass runs only when opposite drawing orders are required.
Persistent cursor buffers retain separate history per layer within the shared allocation limit.

## Downloads, input methods and bidirectional text

Downloads permit 8 MiB of decoded data and one pending application request.
They do not implement the Kitty transfer protocol, resumable transfers or automatic uploads.
IME context does not permit arbitrary surrounding-text edits.
Optional primary-screen bidirectional layout has application-grid exclusions and bounded paragraph processing.
See [bidirectional text](BIDI.md).

## Protocol definitions

- [Kitty relative graphics](https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements).
- [Kitty multiple cursors](https://sw.kovidgoyal.net/kitty/multiple-cursors-protocol/).
- [iTerm2 file transport](https://iterm2.com/documentation-images.html).

## Existing coverage

The suite also checks these features:

- Terminal parsing, grid behavior, scrollback and reflow.
- Sixel.
- Kitty image transmission, placement, animation and Unicode placeholders.
- iTerm2 inline images.
- OSC 66 text sizing.
- Keyboard and mouse encodings.
- Shell markers.
- Clipboard and notification extensions.

See `libs/afterglow-vt/tests` for exact accepted sequences.
Support for a protocol does not imply support for every optional operation.

Run focused tests:

```sh
ctest --test-dir build --output-on-failure -R 'Graphics.Relative|Extensions.Multiple|Extensions.ExtraCursor|Extensions.Download|DesktopText|FileDrop|Render.Multiple|SnapshotCursor|Bidi'
```

Run native acceptance:

```sh
python3 afterglow/tests/desktop_acceptance.py build/afterglow/afterglow
```

Native acceptance requires the [nested-desktop dependencies](DESKTOP.md#native-acceptance-coverage), `python-dbus` and AT-SPI.
It uses temporary configuration and a private session bus.
