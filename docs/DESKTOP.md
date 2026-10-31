<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Desktop integration

## Accessible terminal text

Each visible pane in the current tab is a terminal text object under its window in Qt accessibility and Linux AT-SPI.
Screen readers can query viewport text, UTF-16 cursor and selection offsets, character geometry and text changes.
Text preserves grapheme clusters and soft wraps.
AT-SPI selection changes update the terminal selection.
Tab changes update the exposed pane list. Scrolling updates the text view.
Background tabs and panes hidden by zoom are excluded.
Qt Quick controls expose tab activation and the history scrollbar.

The accessible document contains the viewport. Scroll to read older content.
A cursor outside the viewport is reported as absent.
Terminal output is read-only.
Accessibility cannot rewrite output or move the shell's editing cursor through a document offset.
Rectangular selections report their encompassing range.
Behavior depends on the desktop's AT-SPI service and screen reader.
The acceptance test checks public AT-SPI interfaces. It does not certify every screen reader.

## Input methods and directionality

Preedit text preserves grapheme clusters.
Committed text uses the normal terminal input path.
Focused control editors receive their own composition events.
They report their own surrounding text and cursor geometry.

Input methods can query the live cursor row, surrounding text, UTF-16 cursor offset and cursor rectangle.
These queries work while the viewport is scrolled into history.
Context is read-only. Afterglow rejects surrounding-text replacement requests.
Delete or Backspace cannot safely represent arbitrary replacement ranges in an application-controlled terminal.

Afterglow does not request a Latin language preference.
Predictive text and automatic capitalization remain disabled.
Context contains only displayed cells. Afterglow does not synthesize password contents.

`Terminal.bidi` enables optional bidirectional layout on eligible primary screens.
Selection, pointer targeting and accessible geometry follow visual cell order.
See [bidirectional text](BIDI.md) for choices and limits.

## Drop files into a pane

Drag local files or directories from a file manager onto a pane.
Afterglow focuses that pane and inserts absolute paths quoted for Bash, Zsh and Fish.
Paths use the normal bracketed-paste encoder. Afterglow does not append Enter.
Drops are disabled while a native terminal prompt owns keyboard input or the target is held after exit.

Each drop permits 256 local paths and 1 MiB of quoted UTF-8 text, including separators.
Remote URLs, relative paths and names with control characters reject the entire drop.
A local path dropped into SSH remains a local path. The drop does not upload a file.
Use `scp` or another transfer tool for uploads.

## Receive a file from a terminal application

OSC 1337 `File` and `MultipartFile` downloads open a native Save dialog.
Downloads use `inline=0`, which is also the default.
Choose a destination and select Save, or cancel the request.
Afterglow writes nothing before Save.
The interface supports iTerm2 `it2dl` and SSH transports that pass OSC sequences through.

```sh
python3 data/examples/compatibility/download.py /path/to/report.txt
```

The completed payload is limited to 8 MiB.
Only one request can remain pending across the application.
Afterglow ignores excess requests while the dialog is open.
Invalid base64 and declared size mismatches reject the request.
The suggested name uses only the basename, without control or format characters.
Writes use `QSaveFile`. The native dialog controls overwrite confirmation.
Closing the source pane cancels its dialog.
The protocol has no success acknowledgement.
Afterglow does not open or execute received files automatically.

## Native acceptance coverage

Build the native test target:

```sh
cmake --build build --target afterglow_native_tests
```

Run the suite in an isolated nested KWin desktop:

```sh
python3 afterglow/tests/native_window_acceptance.py --binary build/afterglow/afterglow_native_tests
```

The runner requires X11 or XWayland, nested KWin, D-Bus and XTest.
It selects the Basic control style and generic Qt platform theme to isolate input tests from desktop portal activation.
It sends pointer events to the nested compositor's X11 backend.
The compositor produces Wayland pointer events and negotiates a real Qt `QDrag`.
The test filename contains Unicode and an apostrophe.
The test checks the quoted path received by the PTY.

The suite also checks these behaviors:

- Composition, commit and focused-editor input-method queries.
- Accessible controls and zoom visibility.
- Bidirectional pointer selection.
- Pane launch failures, launch options and close identity.
- Paste confirmation ownership and keyboard acceptance.
- Button key releases.
- Notifications and script text permissions.

Composition tests use Qt events through the window, renderer and PTY.
They do not certify external IBus or Fcitx engines.
The separate `desktop_acceptance.py` suite checks AT-SPI and native file-save dialogs.

## Kitty file transfer

Afterglow accepts the [Kitty file-transfer protocol](https://sw.kovidgoyal.net/kitty/file-transfer-protocol/) on OSC 5113.
A terminal program can send or receive files through its terminal connection, including an SSH connection.

Afterglow asks for permission before a transfer accesses files.
For a receive request, the dialog lists the requested source paths.
For a send request, permission allows the program to create or replace destination files.
Approval applies to one transfer session.

Run these commands on the computer where the terminal program runs:

```sh
# Send a file to the computer that runs Afterglow.
kitten transfer ./report.txt /absolute/destination/report.txt

# Receive a file from the computer that runs Afterglow.
kitten transfer --direction=receive /absolute/source/report.txt ./report.txt

# Update an existing destination with a delta transfer.
kitten transfer --transmit-deltas ./report.txt /absolute/destination/report.txt
```

The protocol supports directories, regular files, symbolic links, hard links, permissions and modification times.
It supports zlib compression, delta transfers, progress reports and cancellation.
Afterglow writes each incoming regular file to a temporary file in its destination directory.
After validation, Afterglow replaces the destination and reports success.
Cancellation removes incomplete temporary files. Completed files remain in place.

Set `Terminal.fileTransferPassword` only when a shared secret must replace the permission dialog.
Pass the same secret to `kitten transfer --permissions-bypass`.
An empty setting always requires permission.

Each pane permits eight sessions and 16,384 files per session.
Individual data packets contain at most 4,096 decoded bytes.
Input and output queues each have a 16 MiB limit.
An exceeded limit stops the affected work. It does not authorize more filesystem access.
File I/O, compression and delta calculation run outside the GUI thread.

Kitty 0.49.2 can omit empty files when it receives a transfer.
Afterglow sends the required end-of-data packet, including for empty files.
Use a Kitty client with the upstream empty-file correction when empty files must be received.

## Kitty drag and drop

Afterglow supports the [Kitty drag-and-drop protocol](https://sw.kovidgoyal.net/kitty/dnd-protocol/) on OSC 72.
Programs can accept desktop drops or supply data for a desktop drag.
The protocol supports MIME selection, copy and move actions, chunked data, thumbnails, remote files and directory handles.

Run a client, then drag data between its terminal pane and another program:

```sh
# Offer files and accept dropped files in the current directory.
kitten dnd ./report.txt ./images/

# Offer text and save dropped text.
kitten dnd --drag text/plain:./source.txt --drop text/plain:./destination.txt
```

A program must enable the protocol before it receives drag events.
A source offer requires a physical left-button drag gesture.
Afterglow rejects file reads before a drop occurs.
It also rejects remote file reads for a drag that starts and ends in the same window.
Programs without protocol support retain the quoted-path paste behavior for local file drops.

Remote source files use a temporary directory until the desktop drag finishes.
Relative symbolic links must remain inside that directory.
Afterglow rejects absolute links and links that escape the temporary directory.

| Resource | Limit |
| --- | --- |
| MIME types per offer | 256 |
| Data per MIME type | 64 MiB |
| Cached MIME data per drag | 256 MiB |
| Remote source data per drag | 1 GiB |
| Remote source entries | 16,384 |
| Open directory handles | 256 |
| Queued read requests | 64 |
| Thumbnail count | 16 |
| Decoded thumbnail data per drag | 64 MiB |
| Thumbnail dimensions | 4,096 × 4,096 pixels |

An exceeded limit cancels the operation with a protocol error.
A stalled request expires after 30 seconds. A stalled remote source preparation expires after 120 seconds.
