<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Desktop integration

## Accessible terminal text

Each pane in the current tab is a terminal text object beneath its window in Qt accessibility / Linux AT-SPI. Screen readers can query viewport text, UTF-16 cursor and selection offsets, character geometry and text changes. Text preserves grapheme clusters and soft wraps. Selecting through AT-SPI updates the real terminal selection. Switching tabs changes the exposed pane list; scrolling changes the text view. Background tabs and panes hidden by zoom are excluded. The same window also exposes its Qt Quick controls, including tab activation and the history scrollbar.

The accessible document is the viewport, not a copy of potentially million-line history. Scroll to read older content. A cursor outside that viewport is reported as absent. Terminal output is read-only: accessibility cannot rewrite output or move a shell's editing cursor by setting a document offset. Rectangular selections currently report their encompassing range. Screen-reader behavior depends on the desktop's AT-SPI service and reader; the automated acceptance test exercises the public AT-SPI interface rather than certifying every screen reader.

## Input methods and directionality

Preedit text preserves grapheme clusters, and committed text reaches the terminal through its normal input path. Focused chrome editors receive their own composition events and report their own surrounding text and cursor geometry. Input methods can request the live cursor row, text before/after the cursor, a UTF-16 cursor offset and cursor rectangle, even while the visible viewport is scrolled back. Context is read-only. Requests to replace surrounding text are rejected; sending Delete or Backspace for an arbitrary replacement range could modify a shell command instead of the displayed text.

The Latin preference hint has been removed so an IME can select its normal language mode. Predictive text and automatic capitalization remain disabled. The terminal does not synthesize password contents: context consists only of cells the application has actually displayed.

Opt-in Unicode bidirectional layout is available through `Terminal.bidi=auto`, `ltr` or `rtl` (`off` is the default). It applies to the primary screen when terminal mouse reporting is disabled. Selection, pointer targeting and accessible geometry follow visual cell order. See [bidirectional text support](BIDI.md) for boundaries and opt-outs.

## Drop files into a pane

Drag local files or directories from a file manager onto a pane. Afterglow focuses the target pane and inserts absolute paths quoted for Bash, Zsh and Fish through the normal bracketed-paste encoder. It never appends Enter. Drops are disabled while a native terminal prompt owns keyboard input or the target is held after exit.

A drop can contain up to 256 local paths and 1 MiB of UTF-8 quoted text, including separators. Remote URLs, relative paths and filenames containing control characters are rejected as a whole. Dropping a local path into SSH inserts that local path; it does not upload the file to the remote machine. Use `scp` or an explicit remote transfer tool for uploads.

## Receive a file from a terminal application

OSC 1337 `File` and `MultipartFile` downloads (`inline=0`, also the default) open a native Save dialog. Choose a destination or cancel. Nothing is written until you choose Save. This is compatible with the iTerm2 `it2dl` protocol and works across SSH transports that pass OSC sequences through.

```sh
python3 data/examples/compatibility/download.py /path/to/report.txt
```

The completed payload is limited to 8 MiB, with one pending request across the application. Excess requests are ignored while that dialog is open. Invalid base64 and declared size mismatches are rejected. The suggested name is reduced to a basename and stripped of control/format characters. Writes use `QSaveFile`; overwrite confirmation belongs to the native dialog. Closing the source pane cancels its pending dialog. There is no success acknowledgement in this OSC protocol and no automatic opening or execution of received files.

## Native acceptance coverage

Build and run the native window suite in an isolated nested KWin desktop:

```sh
cmake --build build --target afterglow_native_tests
python3 afterglow/tests/native_window_acceptance.py --binary build/afterglow/afterglow_native_tests
```

The runner requires a working X11/XWayland display, nested KWin, D-Bus and XTest. It sends pointer events to the nested compositor’s X11 backend, which produces Wayland pointer events and negotiates a real Qt `QDrag` carrying a local filename containing Unicode and an apostrophe. It then checks the quoted path received by the PTY. The suite also exercises composition and commit events, focused-editor input-method queries, accessible chrome and zoom visibility, bidi pointer selection, pane launch failures, launch options, close identity, paste confirmation ownership and keyboard acceptance, button key releases, notifications and script text permissions. All 12 cases passed together during this implementation's native validation.

Composition coverage uses Qt input-method events through the actual window, renderer and PTY; it does not certify an external IBus or Fcitx engine. The separate `desktop_acceptance.py` suite exercises AT-SPI and native file-save dialogs.
