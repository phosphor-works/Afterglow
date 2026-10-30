<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Desktop integration

## Accessible terminal text

Each pane in the current tab is a terminal text object beneath its window in Qt accessibility / Linux AT-SPI. Screen readers can query viewport text, UTF-16 cursor and selection offsets, character geometry and text changes. Text preserves grapheme clusters and soft wraps. Selecting through AT-SPI updates the real terminal selection. Switching tabs changes the exposed pane list; scrolling changes the text view. Background tabs are not included in that window's children.

The accessible document is the viewport, not a copy of potentially million-line history. Scroll to read older content. A cursor outside that viewport is reported as absent. Terminal output is read-only: accessibility cannot rewrite output or move a shell's editing cursor by setting a document offset. Rectangular selections currently report their encompassing range. Screen-reader behavior depends on the desktop's AT-SPI service and reader; the automated acceptance test exercises the public AT-SPI interface rather than certifying every screen reader.

## Input methods and directionality

Preedit text and committed text are supported. Input methods can request the live cursor row, text before/after the cursor, a UTF-16 cursor offset and cursor rectangle, even while the visible viewport is scrolled back. Context is read-only. Requests to replace surrounding text are rejected; sending Delete or Backspace for an arbitrary replacement range could modify a shell command instead of the displayed text.

The Latin preference hint has been removed so an IME can select its normal language mode. Predictive text and automatic capitalization remain disabled. The terminal does not synthesize password contents: context consists only of cells the application has actually displayed.

Full bidirectional layout is not implemented. HarfBuzz glyph shaping does not imply Unicode bidirectional reordering. See [the separately scoped bidi design](BIDI.md) before treating Arabic/Hebrew mixed-direction output as supported visual ordering.

## Drop files into a pane

Drag local files or directories from a file manager onto a pane. Afterglow focuses the target pane and inserts absolute paths quoted for Bash, Zsh and Fish through the normal bracketed-paste encoder. It never appends Enter. Drops are disabled while a native terminal prompt owns keyboard input or the target is held after exit.

A drop can contain up to 256 local paths and 1 MiB of quoted text. Remote URLs, relative paths and filenames containing control characters are rejected as a whole. Dropping a local path into SSH inserts that local path; it does not upload the file to the remote machine. Use `scp` or an explicit remote transfer tool for uploads.

## Receive a file from a terminal application

OSC 1337 `File` and `MultipartFile` downloads (`inline=0`, also the default) open a native Save dialog. Choose a destination or cancel. Nothing is written until you choose Save. This is compatible with the iTerm2 `it2dl` protocol and works across SSH transports that pass OSC sequences through.

```sh
python3 data/examples/compatibility/download.py /path/to/report.txt
```

The completed payload is limited to 8 MiB, with one pending request across the application. Excess requests are ignored while that dialog is open. Invalid base64 and declared size mismatches are rejected. The suggested name is reduced to a basename and stripped of control/format characters. Writes use `QSaveFile`; overwrite confirmation belongs to the native dialog. Closing the source pane cancels its pending dialog. There is no success acknowledgement in this OSC protocol and no automatic opening or execution of received files.
