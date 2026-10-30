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
