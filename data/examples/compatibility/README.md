<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Compatibility examples

Run these inside Afterglow; they do not modify your configuration.

```sh
python3 data/examples/compatibility/relative-images.py
python3 data/examples/compatibility/multi-edit.py
python3 data/examples/compatibility/download.py /path/to/file.txt
```

The first animates a parent image while its child follows the declared offset. The second edits three ASCII values together using native block, beam and underline cursors. Tab cycles RGB, palette, reversed-cell and main-cursor colors; Escape exits. The colors apply to every extra cursor, as defined by the protocol. Both restore the normal screen on exit. The third offers a file through OSC 1337, including across SSH; Afterglow asks where to save it.

The image and cursor clients also demonstrate the corresponding Kitty protocols. The download format is shared with iTerm2's `it2dl`. See [supported operations and limits](../../../docs/COMPATIBILITY.md).

For a minimal color example inside an application-owned screen:

```python
# Set extra-cursor fill to turquoise and text to dark blue.
print("\033[>40;2:80:220:200 q\033[>30;2:12:24:36 q", end="")
# Place a block cursor at row 3, column 11 (coordinates start at 1).
print("\033[>1;2:3:11 q", end="", flush=True)
# Clear positions and restore colors when the application exits.
print("\033[>0;4 q\033[>30;0 q\033[>40;0 q", end="", flush=True)
```

A protocol client can query capabilities with `CSI > SPACE q`, positions with `CSI > 100 SPACE q`, and colors with `CSI > 101 SPACE q`. Read those responses from terminal input; do not mistake them for user keystrokes. The interactive demo sends the documented operations directly and expects a terminal supporting them.
