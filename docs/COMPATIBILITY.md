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
| Local file drop | Local absolute paths quoted for Bash/Zsh/Fish, targeted pane, bracketed paste, no automatic Enter | `FileDrop.*`; [file-drop workflow](DESKTOP.md#drop-files-and-text-into-a-pane) |
| Text drop and selection drag | Dropped text pasted with the size limit and the question of a paste, selected text dragged to another pane or application (`Mouse.dragSelection`) | `NativeWindow.Dropped*`, `NativeWindow.DropsThatAPaneRefusesPasteNothing`, `NativeWindow.ASelection*`, `NativeWindow.*InsideTheSelection*`; [text drops](DESKTOP.md#drop-files-and-text-into-a-pane), [selection drag](INPUT.md#drag-selected-text-out-of-a-pane) |
| Kitty indexed images | `f=8` and `f=16`; the special indices 248 to 255; a new color of the pane draws the image again; no frames | `Graphics.Indexed*`, `Render.IndexedImages*`, `SnapshotImages.Indexed*`; [indexed images](#indexed-images) |

## Relative images

Relative placement chains permit 32 parent links.

The main screen and the alternate screen have separate image stores.
Each store has these limits:

| Item | Limit | At the limit |
| --- | --- | --- |
| Images | 4,096 | Afterglow deletes an image without a placement: first an image with the transient hint (`N=1`), then the oldest. When every image has a placement, a Kitty command gets an `ENOSPC` error. |
| Placements | 4,096 | A Kitty command gets an `ENOSPC` error. A new Sixel or iTerm2 image removes the oldest Sixel or iTerm2 placement. When there is none, Afterglow does not keep the new image. |
| Frames per image | 1,024 | A Kitty command gets an `ENOSPC` error. |
| Frames of all images | 16,384 | Afterglow deletes images as for image data. A frame of one pixel costs far more to keep than its 4 bytes. |
| Image data | `Graphics.quota`, 320 MiB by default, at 4 bytes for each pixel | Afterglow deletes images without a placement: first those with the transient hint (`N=1`), then the oldest. When none is left, it deletes the oldest image with a placement. |
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
| ED 22 (move the screen into the scrollback, then erase it, as in Kitty) | The lines down to the last line with text go into the scrollback with their images. Then ED 2 erases the screen. On the alternate screen, or with a scrolling region, ED 22 is the same as ED 2. |
| ED 3 (erase the scrollback) | Images whose top row is in the scrollback go. Images on the screen stay, like the text. |
| EL (erase part of a line) | An image whose rows include that line goes. |
| ED 0 and ED 1 (erase to the end or to the start of the screen) | An image whose rows include an erased line goes. |
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

Afterglow deletes a temporary file (`t=t`) only when it is below `/tmp`, `/dev/shm` or `$TMPDIR`, and its path below that directory holds `tty-graphics-protocol`.
Afterglow keeps every other file that a command names.
Afterglow deletes the shared memory object of a command (`t=s`) when it has read it or refused the command, whatever its name, as the Kitty specification says.
`kitten icat` names its objects `/icat-` and a random suffix.

A read from a file or shared memory that fails gives the error `EBADF:failed to read image file`.
These cases give that error:

- The file or object does not exist, or it is not a regular file.
- The file is in `/proc`, `/sys` or `/dev`, except `/dev/shm`.
- The name is longer than 4,096 bytes.
- The file holds less data than `s` × `v` raw or indexed pixels.
- The data is PNG (`f=100`) or compressed (`o=z`), and the part to read is longer than 320 MiB.

The main screen and the alternate screen of a pane each have their own image work budget.
The budget is 1 GiB, and it fills again at 1 GiB each second.

| Work over the budget | Error |
| --- | --- |
| A read from a file (`t=f`), a temporary file (`t=t`) or shared memory (`t=s`) | `EBUSY:too much image data read` |
| A frame edit (`a=f`, `a=c`) | `EBUSY:too much frame editing` |

The error ends with `, try again later` when the program can send the same command again.
When Afterglow deletes the temporary file or the shared memory object by the rules above, the error has no `, try again later`.
A command that tries again with the same name then gets `EBADF:failed to read image file`.

Afterglow reads the file in the thread that reads the program's output.
While a read waits, for example on a network mount that does not answer, the pane does not change.
To prevent this, set `Terminal.graphicsFileTransfer` to `"deny"`.

A feature query (`a=q` with `e`) gets the keys of that action that Afterglow supports.
The transient usage hint (`N=1`) applies to a transmission (`a=t`, `a=T`). It does not apply to a frame (`a=f`).
When Afterglow must delete an image to make room, it first deletes an image with the hint that has no placement.
A feature query reports `N` for the transmit and query actions.
The delete commands `d=f` and `d=F` delete one frame of an animation for each command.
The `r` key selects the frame (the first frame is 1). Without `r`, the command deletes the first frame.
On an image with one frame, `d=F` deletes the image, and `d=f` keeps it.

Afterglow checks the size that a Kitty transmission declares before it reads or decompresses the data.
Raw pixels (`f=24` or `f=32`) use 3 or 4 bytes for each pixel, and Afterglow keeps 4.
Indexed pixels (`f=8` or `f=16`) use 1 or 2 bytes for each pixel, and Afterglow keeps the same number.
The limits count 4 bytes for each pixel of all these formats, because Afterglow draws every image as RGBA.

| Case | Result |
| --- | --- |
| `o` has a value that is not `z` | The program gets an `EINVAL` error. |
| `f=0` | Afterglow reads it as the default, `f=32`, as Kitty does. |
| `f` is not 0, 8, 16, 24, 32 or 100 | The program gets an `EINVAL` error. |
| Raw or indexed pixels without `s` or `v`, or with a value above 10,000 | The program gets an `EINVAL` error. |
| Raw or indexed pixels where `s` × `v` × 4 bytes is more than 320 MiB | The program gets an `EFBIG` error. |
| Raw or indexed pixels where `s` × `v` × 4 bytes is more than `Graphics.quota` | The program gets an `ENOSPC` error. |
| Uncompressed raw or indexed pixels from a file or shared memory | Afterglow reads `s` × `v` pixels and no more of the file. |
| Raw or indexed pixels with `o=z` that decompress to more than `s` × `v` pixels | The program gets an `EINVAL` error. |
| PNG data (`f=100`) or `o=z` data that is longer than 320 MiB, inline | The program gets an `EFBIG` error. |
| PNG data with `o=z` that decompresses to more than `Graphics.quota` or 320 MiB | The program gets an `EINVAL` error. |
| A PNG image whose decoded pixels (width × height × 4 bytes) are more than 320 MiB | The program gets an `EBADPNG` error. |
| A PNG image whose decoded pixels (width × height × 4 bytes) are more than `Graphics.quota` | The program gets an `ENOSPC` error. |

## Indexed images

An indexed image (`f=8` or `f=16`) holds palette indices, so it follows the colors of the pane.
When the colors of the pane change, Afterglow draws the image again in the new colors.
A theme, OSC 4 and OSC 21 are examples of such a change.
With `f=8`, every pixel is opaque. With `f=16`, the second byte of each pixel is its alpha.

| Index | Color |
| --- | --- |
| 0 to 247 | Entries 0 to 247 of the 256-color palette |
| 248 | The default background. `Window.backgroundOpacity` does not apply to it. |
| 249 | The default foreground |
| 250 | The selection background, or the default foreground when no setting or program sets it |
| 251 | The selection foreground, or the default background when no setting or program sets it |
| 252 | The cursor color, or the default foreground when no setting or program sets it |
| 253 | The cursor text color, or the default background when no setting or program sets it |
| 254 | The default foreground. Afterglow underlines a link under the pointer in its text color. Afterglow has no link hover color. |
| 255 | The default foreground (reserved) |

In reverse video (DECSCNM), 248 and 249 change places, as the default colors do.
An indexed image has one frame.
A frame action (`a=f`) or a composition (`a=c`) on an indexed image gets the error `EINVAL:indexed images have no frames`.
A frame action with `f=8` or `f=16` on an image that exists gets the same error.
A feature query reports `f=8` and `f=16` for the transmit and query actions.

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
Query 100 gives the cursors as rectangles (type 4). A run of cursors on one row is one rectangle.
Equal runs on the rows below join it. A cursor alone is a point (type 2).

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
OSC 8 links of each screen have a 16 MiB limit. Above it, new text prints without a link.
Each link counts its URI, its id and 160 bytes for its entry. A link with an id counts its URI and id two times.
An OSC 8 sequence longer than 8,192 bytes ends the open link. The text after it has no link.
OSC 0 and OSC 2 titles lose C0, DEL and C1 controls, U+2028, U+2029 and the controls of bidirectional text.
A byte that is not UTF-8 becomes U+FFFD.
A C1 control sent as UTF-8 (U+0080 to U+009F) prints nothing.

OSC 133 marks have no length limit. A `cmdline` or `cmdline_url` value over 65,536 bytes is ignored, and the mark still counts.
The command line is then the text typed after the `B` mark.
The command line keeps no control characters except line feeds and tabs. A byte that is not UTF-8 becomes U+FFFD.
A command that erases the whole screen (ED 2) moves the start of its output to the top of the screen.
OSC 3008 contexts form a stack. A `start` for a context that is open ends the contexts inside it.
A terminal reset (RIS or the Reset command) keeps the context stack, as the [context specification](https://uapi-group.org/specifications/specs/osc_context/) requires.

## Downloads, input methods and bidirectional text

Downloads permit 8 MiB of decoded data and one pending application request.
A `MultipartFile` download stops when its data passes the 8 MiB limit. An inline image in parts can have 96 MiB of base64 data.
`MultipartFile` arguments have a 4,096-byte limit. Longer arguments refuse the file.
A new `MultipartFile`, a terminal reset or the end of the pane's program ends a file that has no `FileEnd`.
A new shell prompt is a prompt mark (OSC 133 `A` or `N`, or OSC 633 `A`) after a command start mark (`C`).
One new prompt does not end the file, because a job in the background can still send it.
A second new prompt with no `FilePart` after the first ends it, because the program that sent it has stopped.
A prompt mark with no `C` before it does not count, because a shell sends it each time it draws the same prompt again.
A command end mark (`D`) does not make a new prompt.
The Bash and Zsh integration of Afterglow sends it after each command, also after Enter on an empty line.
With a shell that sends no `C` mark, no prompt ends a file.
A new `MultipartFile`, the size limit, a terminal reset or the end of the pane's program still ends it.
In a mux session, a download goes to the window that shows the session, with the other requests of the session. Without a window, the mux session drops it (see [requests that wait](MUX.md#requests-that-wait)).
OSC 1337 downloads are not resumable, and they do not upload files.
Afterglow supports the Kitty file-transfer protocol separately. See [Kitty file transfer](DESKTOP.md#kitty-file-transfer).
IME context does not permit arbitrary surrounding-text edits.
Optional primary-screen bidirectional layout has application-grid exclusions and bounded paragraph processing.
See [bidirectional text](BIDI.md).

## Character widths

The widths come from Unicode 18.0.
Afterglow joins characters into grapheme clusters as UAX #29 says, when grapheme clustering (mode 2027) is on, which is the default.
A cluster takes the cells that glibc's `wcwidth` gives its characters, but no more than two.
Shells and editors measure text with `wcwidth`, so the cursor stays where they expect it.

| Text | Cells | Kitty |
| --- | --- | --- |
| A wide emoji, then VS15 (U+FE0E) | 1 | 1 |
| A text emoji, then VS16 (U+FE0F) | 2 | 2 |
| A letter and a spacing mark (Hindi का, Tamil கா) | 2 | 1 |
| A conjunct: a consonant, a virama and a consonant (Hindi क्र) | 2 | 1 |
| Thai SARA AM or Lao AM after a letter | 2 | 2 |
| A prepended mark (U+0600 to U+0605, U+06DD, U+070F, U+0890, U+0891, U+08E2, U+110BD, U+110CD) | 1 | 0 |

`tools/conformance/ucs-detect.sh` measures the widths with ucs-detect. See [TESTING.md](TESTING.md#character-widths-with-ucs-detect).

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

## aglow interoperability

`aglow` is a client of four Kitty protocols: file transfer, drag and drop, graphics and the clipboard. It does not need Kitty.
`aglow run`, which runs the commands of scripts, uses no Kitty protocol and has no counterpart in `kitten`.
When `AFTERGLOW_SOCKET` names no socket on its own computer, `aglow run` asks the terminal for the socket of an ssh session.
It asks with [OSC 24569](#remote-commands-osc-24569), which is Afterglow's own.
[The aglow client](DESKTOP.md#the-aglow-client) tells how to use it.
Its message formats come from `libs/afterglow-proto`, the same code that the terminal uses.

| Command | Protocol | Interoperability checks |
| --- | --- | --- |
| `aglow transfer` | File transfer (OSC 5113): directories, links, permissions, times, zlib, deltas and the SHA-256 password form | Against the Afterglow service in process (`Client.*`). Through a pseudo-terminal, also with `--confirm-paths`. Against Kitty 0.49.2 (`kitty_transfer_interop.py`). Against a hostile terminal (`Proto.Hostile.*`) |
| `aglow dnd` | Drag and drop (OSC 72): MIME data, copy and move, remote directory trees in both directions | Against Afterglow in a nested KWin (`NativeWindow.Aglow*`). As the target or source of a remote drag (`AglowDrop.*`, `AglowDrag.*`). With the question that lists the files before a drop replaces them (`AglowOverwrite.*`) |
| `aglow icat` | Graphics (APC G): PNG as it is, other formats as pixels, GIF animation frames, Unicode placeholders, tmux passthrough | `aglow_icat_render` draws its output with the Afterglow renderer. `aglow_icat_decode` checks the pixels of crafted images, and the first frame of a GIF over the 320 MiB budget. `aglow_tmux_pty` runs it in a private tmux server, with `allow-passthrough` on and off. |
| `aglow run` | The socket query of a remote host (OSC 24569), with the DA1 guard and tmux passthrough | Against a terminal and a socket that a test plays (`aglow_run_pty`). Also in a private tmux server (`aglow_tmux_pty`). Through sshd run as the user (`RemoteCommandsTest.*`) |
| `aglow clipboard` | Clipboard (OSC 5522), and OSC 52 for plain text in a terminal without OSC 5522 | Against a terminal that a test plays (`aglow_clipboard_pty`, `aglow_clipboard_cases`), and the message formats in `libs/afterglow-proto/tests` |

`aglow transfer`, `aglow dnd`, `aglow icat` and `aglow clipboard` take the options of the `kitten` command of the same name, with the differences below.

- `aglow transfer` sends the permission password only in the SHA-256 form, which Afterglow and Kitty accept. It does not use Kitty's own form, which encrypts the password with `KITTY_PUBLIC_KEY`.
- `aglow icat` decodes images with stb_image. It does not use ImageMagick or download images from URLs.
- The `--engine` option of `aglow icat` takes `auto` and `builtin`, which mean the same. It refuses `magick`.
- `aglow icat` accepts `--silent` and ignores it, as `kitten icat` does.
- `aglow icat` compresses pixels with zlib only when it sends them through the terminal connection. A file or shared memory holds raw pixels.
- `aglow clipboard` does not convert between image formats when it reads the clipboard.
- `aglow dnd` has the option `--confirm-drop-timeout`, which `kitten dnd` does not have.

Afterglow adds the `o` key to the end message of a drag (`t=e:x=4`). It names the action that the drop took.
Kitty does not send it.
A move to another application is reported as a copy, so that the source keeps its files. See [Kitty drag and drop](DESKTOP.md#kitty-drag-and-drop).

Run the tests of `aglow`:

```sh
ctest --test-dir build --output-on-failure -R 'aglow|Aglow|^(Client|RunBinaryTest|RemoteCommandsTest)\.|^Proto\.(Client|Hostile|RunClient|RunInput|RunRemote|Remote)\.'
```

## kitten interoperability

The `kitten` commands of Kitty are clients of the same protocols as `aglow`.
The tests below run `kitten` 0.50 against Afterglow. Each test skips when `kitten` is not installed.

| Command | Interoperability checks |
| --- | --- |
| `kitten icat` | `afterglow_kitten_icat` runs it in `afterglow_headless`, which answers its queries, and reads the images and placements that reach the terminal. It checks the transfer modes `detect`, `stream`, `file` and `memory`, with a PNG that `kitten` sends as it is and with an image that it converts. Each temporary file and shared memory object must be deleted. It also checks `--place`, Unicode placeholders, the frames of an animated GIF, and `kitten` in a private tmux server. Then it draws the output with the Afterglow renderer. |
| `kitten clipboard` | `NativeWindow.KittenClipboard*` run it in a pane of a window in a nested KWin: a write of text and of several MIME types, a read that the prompt allows or denies, and a read over the 8 MiB limit (`EFBIG`). |
| `kitten dnd` | `NativeWindow.Kitty*` drag and drop tests, in a nested KWin with real compositor input: a drag between panes, remote directory trees, and a move or copy to a Wayland application. |
| `kitten transfer` | `afterglow_transfer_kitten_relay`, against the Afterglow file transfer service in process. |

## Remote commands (OSC 24569)

OSC 24569 is Afterglow's own sequence. It tells `aglow run` on a remote host where the socket of its ssh session is.
The number spells AGLOW on a phone keypad. No other terminal is known to use it.
`libs/afterglow-proto` writes and reads the query and the replies, for the pane and for `aglow`.

| Sequence | Form |
| --- | --- |
| Query, from `aglow run` | `OSC 24569 ; q=remote ST` |
| Reply with a socket | `OSC 24569 ; path=PATH ST` |
| Reply without a socket | `OSC 24569 ; error=REASON ST` |

| Reason | Meaning |
| --- | --- |
| `off` | The pane has no forward. It runs no ssh session, or a requirement of [remote commands](CONTROL.md#remote-commands) is missing |
| `forward` | The ssh server refused the forward, or Afterglow did not confirm the forward in time |
| `version` | The `aglow` on the remote host has another protocol |
| `length` | The socket path on the remote host would be over 103 bytes |

`PATH` is an absolute path of at most 103 bytes, with no control characters.
A reply ends with BEL when the query ended with BEL, else with ST.
The pane answers at once, before the replies to what follows the query.
Afterglow ignores every other OSC 24569, such as a reply that a program echoes.

`aglow run` sends DA1 (`CSI c`) after the query, and every terminal answers DA1.
When the DA1 answer comes with no OSC 24569 reply, the terminal is not Afterglow.
`aglow run` waits at most 2 seconds, as a last guard.
Inside tmux, `aglow run` sends the query in tmux passthrough (`DCS tmux; ... ST`).
tmux answers DA1 itself at once, and keeps the DA1 answer of the terminal.
Thus inside tmux the guard is a status report (`CSI 5 n`) in the same passthrough. tmux passes its answer (`CSI 0 n`) to the pane.
When `allow-passthrough` is off, tmux passes nothing, and `aglow run` asks tmux for that option first.
tmux gets half of the wait for its answer, and the query gets the rest. When tmux answers later, `aglow run` sends no query. The query goes through the same tmux server, which is slow to answer, and a late reply would go to the shell.
`aglow run` asks the terminal only when standard output is its terminal and its process group is in the foreground there.
When its output goes into a pipeline, the program at the end of the pipeline can read the terminal and take the reply. Then `aglow run` asks nothing.
A background job asks nothing, because a change of the terminal modes would stop it (`SIGTTOU`).
A program that writes into `aglow run` through a pipe and reads the terminal can still take the reply.
`aglow run` reads all the input of the terminal up to the guard's answer.
Without that answer, it discards the input and output of the terminal that wait, so that a late reply does not go to the shell.

The reply is not a secret. Any remote program can send the query.
When no program reads the reply, it goes to the shell as input.
The permissions of the socket are the only boundary.

## Protocol definitions

- [Kitty relative graphics](https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements).
- [Kitty indexed color data](https://sw.kovidgoyal.net/kitty/graphics-protocol/#indexed-color-data).
- [Kitty multiple cursors](https://sw.kovidgoyal.net/kitty/multiple-cursors-protocol/).
- [iTerm2 file transport](https://iterm2.com/documentation-images.html).

## Existing coverage

The suite also checks these features:

- Terminal parsing, grid behavior, scrollback and reflow.
- Tab stops: HTS, TBC, CHT, CBT and DECST8C (`CSI ? 5 W`), which sets a stop at every 8th column again.
- Sixel.
- Kitty image transmission, placement, animation, Unicode placeholders and indexed images.
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
ctest --test-dir build --output-on-failure -R 'Graphics.Relative|Graphics.TransmitAndPlaceReportsInvalidRelativeParent|Extensions.Multiple|Terminal.ExtraCursorsAreBounded|Terminal.ResizeClipsExtraCursors|Extensions.ResizeClipsExtraCursors|Extensions.ExtraCursor|Extensions.Download|DesktopText|FileDrop|Render.Multiple|SnapshotCursor|Bidi|Graphics.Indexed|Render.Indexed|SnapshotImages.Indexed'
```

See [accessibility and downloads](TESTING.md#accessibility-and-downloads) for the native acceptance suite.
