<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Terminal compatibility

Afterglow targets Linux and Wayland with Vulkan.
The terminal core in `libs/afterglow-vt` parses the protocols.
The message formats of file transfer, drag and drop, graphics and clipboard are in `libs/afterglow-proto`.
Renderer tests also check GPU pixels.
The following table describes tested features. It does not claim complete xterm, Kitty or iTerm2 compatibility.

## Supported protocol features

| Feature | Supported behavior | Regression and interoperability fixture |
| --- | --- | --- |
| Kitty relative images | `P`, `Q`, signed `H`/`V`; parent movement; chained parents; cascade deletion; missing-parent, cycle and depth errors; virtual placeholder parents | `Graphics.Relative*`, `Graphics.TransmitAndPlaceReportsInvalidRelativeParent` and transmit/placeholder pixel regressions; [moving-image client](../data/examples/compatibility/relative-images.py) |
| Kitty multiple cursors | Shapes 1/2/3/29, clear 0, coordinate types 0/2/4, color operations 30/40, capabilities and queries 100/101; clipping, reset, clear, alternate-screen lifecycle; blink and mixed shape rendering | `Extensions.MultipleCursor*`, `Extensions.ExtraCursorColors*`, `Extensions.ResizeClipsExtraCursorsToTheScreen`, `Terminal.ExtraCursorsAreBounded`, `Terminal.ResizeClipsExtraCursorsToTheClampedSize`, `SnapshotCursor.*`, `Render.MultipleCursors*`; [multi-edit client](../data/examples/compatibility/multi-edit.py) |
| OSC 1337 downloads | `File`, `MultipartFile`, `FilePart`, `FileEnd`; explicit native Save dialog; cancellation; bounded data | `Extensions.Downloads*`, `desktop_acceptance.py`; [download client](../data/examples/compatibility/download.py), upstream [it2dl](https://iterm2.com/utilities/it2dl) |
| IME context | Live cursor-row surrounding text, before/after text, UTF-16 position and cursor rectangle | `DesktopText.*`; [IME behavior](DESKTOP.md#input-methods-and-directionality) |
| Native terminal accessibility | Viewport text, cursor, selection and character geometry through Qt/AT-SPI; pane actions, the pane menu and docked-area buttons | `DesktopText.*`, `NativeWindow.Accessible*`, `desktop_acceptance.py`; [accessibility](DESKTOP.md#accessible-terminal-text) |
| Local file drop | Local absolute paths quoted for Bash/Zsh/Fish, targeted pane, bracketed paste, no automatic Enter | `FileDrop.*`; [file-drop workflow](DESKTOP.md#drop-files-into-a-pane) |

## Relative images

Relative placement chains permit 32 parent links.

The main screen and the alternate screen have separate image stores.
Each store has these limits:

| Item | Limit | At the limit |
| --- | --- | --- |
| Images | 4,096 | Afterglow deletes the oldest image without a placement. When every image has a placement, a Kitty command gets an `ENOSPC` error. |
| Placements | 4,096 | A Kitty command gets an `ENOSPC` error. A new Sixel or iTerm2 image removes the oldest Sixel or iTerm2 placement. When there is none, Afterglow does not keep the new image. |
| Frames per image | 1,024 | A Kitty command gets an `ENOSPC` error. |
| Image data | `Graphics.quota`, 320 MiB by default | Afterglow deletes the oldest image without a placement, then the oldest image. |
| Pending transmissions | 320 MiB | The transmission fails with an `EFBIG` error. |

An omitted parent placement ID selects the first matching placement at creation.
Displayed Unicode placeholders determine a virtual parent's origin, including implied diacritics.
Afterglow hides the children when it displays no parent placeholders.
Scrolling or resizing can change the anchor.
Parent offsets use cells. `X` and `Y` remain the child's pixel offsets.
Relative placements do not move the terminal cursor.

## Image lifetime and scrolling

| Event | Result |
| --- | --- |
| Lowercase Kitty delete (`d=a`, `d=i` and the others) | The placements go. Image data with an ID or a number stays, also the data of child images. |
| Last placement of an image without an ID or a number goes | The image data goes. Sixel and iTerm2 images do the same. |
| ED 2 (erase the screen) | Images that reach into the screen go, also when their top row is in the scrollback. |
| ED 3 (erase the scrollback) | Images whose top row is in the scrollback go. Images on the screen stay, like the text. |
| SU, SD, IL, DL, or a line feed or reverse index at a scroll-region margin | An image whose top-left cell is in the region moves with its rows. It shows only its rows that are in the region after the scroll. It goes when none of its rows is in the region. Rows below the last row of the screen stay when the scroll is up: they come into view as the image moves. A relative child image moves with its parent, and the same rule cuts it. It goes when its parent goes. |

The same scrolls move shell-integration marks (OSC 133) with their rows.
A command or prompt region goes when its start or end leaves the scroll region.
A selection moves when a full-width scroll region holds all of it.
Afterglow clears the selection when the scroll region holds only part of it.
It also clears the selection when a region with left and right margins holds any of it.
With left and right margins, only images and marks in those columns move.

Afterglow stores an iTerm2 inline image at its own size, or smaller when the requested size is smaller.
The renderer enlarges it to the requested size.

To refuse Kitty transmissions from files, temporary files and shared memory (`t=f`, `t=t`, `t=s`), set `Terminal.graphicsFileTransfer` to `"deny"`.
The program then gets an `EPERM` error, and Afterglow opens no file.
Inline transmissions (`t=d`) always work.

Afterglow checks the size that a Kitty transmission declares before it reads or decompresses the data.
Raw pixels (`f=24` or `f=32`) use 3 or 4 bytes for each pixel.

| Case | Result |
| --- | --- |
| `o` has a value that is not `z` | The program gets an `EINVAL` error. |
| `f` is not 24, 32 or 100 | The program gets an `EINVAL` error. |
| Raw pixels without `s` or `v`, or with a value above 10,000 | The program gets an `EINVAL` error. |
| Raw pixels where `s` × `v` × 4 bytes is more than 320 MiB | The program gets an `EFBIG` error. |
| Raw pixels where `s` × `v` × 4 bytes is more than `Graphics.quota` | The program gets an `ENOSPC` error. |
| Uncompressed raw pixels from a file or shared memory | Afterglow reads `s` × `v` pixels and no more of the file. |
| Raw pixels with `o=z` that decompress to more than `s` × `v` pixels | The program gets an `EINVAL` error. |
| PNG data (`f=100`) or `o=z` data that is longer than 320 MiB, inline or from a file or shared memory | The program gets an `EFBIG` error. |
| PNG data with `o=z` that decompresses to more than `Graphics.quota` or 320 MiB | The program gets an `EINVAL` error. |
| A PNG image whose decoded pixels (width × height × 4 bytes) are more than 320 MiB | The program gets an `EBADPNG` error. |
| A PNG image whose decoded pixels (width × height × 4 bytes) are more than `Graphics.quota` | The program gets an `ENOSPC` error. |

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
The terminal keeps at most 4,096 extra cursors. It ignores new positions after this limit.
At this limit, the terminal keeps the positions of the rectangles that start nearest the top, then nearest the left.
The order of the rectangles in the sequence does not matter.
Full reset restores default colors. Clear and screen switches remove positions.
Positions use screen coordinates and do not scroll with output.
Cursors disappear while viewing scrollback. Resize clips positions to the screen.

Separate layers keep block text readable and beams above glyphs.
With layer shaders, mixed cursor order requires one additional RGBA8 frame texture.
This costs approximately 7.9 MiB at 1080p or 31.6 MiB at 4K.
A second cursor shader pass runs only when opposite drawing orders are required.
Persistent cursor buffers retain separate history per layer within the shared allocation limit.

## Notifications, links and shell contexts

OSC 99 notification bodies can contain more than one line. Tabs become spaces.
OSC 9 and OSC 777 notification text gets the same check as OSC 99 text.
The text must be valid UTF-8 with no C0, DEL or C1 control characters.
Afterglow ignores a notification that fails the check.
A base64 payload can be divided at any character between chunks.
OSC 8 link text (URIs and ids) of each screen has a 16 MiB limit. Above it, new text prints without a link.
OSC 3008 contexts form a stack. A `start` for a context that is open ends the contexts inside it.
A terminal reset (RIS or the Reset command) keeps the context stack, as the [context specification](https://uapi-group.org/specifications/specs/osc_context/) requires.

## Downloads, input methods and bidirectional text

Downloads permit 8 MiB of decoded data and one pending application request.
They do not implement the Kitty transfer protocol, resumable transfers or automatic uploads.
IME context does not permit arbitrary surrounding-text edits.
Optional primary-screen bidirectional layout has application-grid exclusions and bounded paragraph processing.
See [bidirectional text](BIDI.md).

## Kitty color control and pointer shapes

OSC 21 supports the [Kitty color-control protocol](https://sw.kovidgoyal.net/kitty/color-stack/).
Programs can set, query and reset palette colors and dynamic colors.
Supported values include named X11 colors, hexadecimal colors, `rgb:` values and `rgbi:` values.
Cursor text, visual-bell color and eight transparent-background slots affect rendering.
Transparent slots support an explicit opacity or the configured window opacity.
The color stack saves and restores these settings.
Unknown color keys produce the protocol's encoded `unknown` response.

OSC 22 supports the [Kitty mouse-pointer protocol](https://sw.kovidgoyal.net/kitty/pointer-shapes/).
Programs can replace, push, pop and query shapes.
The main screen and alternate screen have separate stacks, each with 32 entries.
A full stack discards its oldest entry when a new entry is pushed.
All protocol shape names are supported, including zoom, cell and vertical-text pointers.
A terminal reset clears both stacks and restores the default text pointer.

## The aglow client

`aglow` is the client side of the Kitty protocols that Afterglow supports. It does not need Kitty.
Its message formats come from `libs/afterglow-proto`, the same code that the terminal uses.

| Command | Protocol | Interoperability checks |
| --- | --- | --- |
| `aglow transfer` | File transfer (OSC 5113): directories, links, permissions, times, zlib, deltas and the SHA-256 password form | Against the Afterglow service in process and through a pseudo-terminal, and against Kitty 0.49.2 (`kitty_transfer_interop.py`) |
| `aglow dnd` | Drag and drop (OSC 72): MIME data, copy and move, remote directory trees in both directions | Against Afterglow in a nested KWin (`NativeWindow.Aglow*`), and as the target of a remote drop (`AglowDrop.*`) |
| `aglow icat` | Graphics (APC G): PNG as it is, other formats as pixels with zlib, GIF animation frames, Unicode placeholders, tmux passthrough | `aglow_icat_render` draws its output with the Afterglow renderer |
| `aglow clipboard` | Clipboard (OSC 5522), and OSC 52 for plain text in a terminal without OSC 5522 | Message formats in `libs/afterglow-proto/tests` |

`aglow` takes the options of the `kitten` command of the same name.
`aglow transfer` sends the permission password only in the SHA-256 form, which Afterglow and Kitty accept.
Kitty's own form, which encrypts the password with `KITTY_PUBLIC_KEY`, is not used.
`aglow icat` decodes images with stb_image. It does not use ImageMagick or download images from URLs.
`aglow clipboard` does not convert between image formats when it reads the clipboard.

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
See `libs/afterglow-proto/tests` for the message formats.
Support for a protocol does not imply support for every optional operation.

Run focused tests:

```sh
ctest --test-dir build --output-on-failure -R 'Graphics.Relative|Graphics.TransmitAndPlaceReportsInvalidRelativeParent|Extensions.Multiple|Terminal.ExtraCursorsAreBounded|Terminal.ResizeClipsExtraCursors|Extensions.ResizeClipsExtraCursors|Extensions.ExtraCursor|Extensions.Download|DesktopText|FileDrop|Render.Multiple|SnapshotCursor|Bidi'
```

See [accessibility and downloads](TESTING.md#accessibility-and-downloads) for the native acceptance suite.
