<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Compatibility examples

These demonstration clients show the image, multiple-cursor and download protocols that Afterglow supports.
Run them inside Afterglow. They do not change your configuration.

```sh
python3 examples/compatibility/relative-images.py
python3 examples/compatibility/multi-edit.py
python3 examples/compatibility/download.py /path/to/file.txt
```

`relative-images.py` moves a parent image while its child follows the declared offset.
`multi-edit.py` edits three ASCII values with block, beam and underline cursors.
Tab selects RGB, palette, reversed-cell or main-cursor colors. Escape exits.
Colors apply to all extra cursors, as the protocol requires.
Both clients restore the normal screen on exit.
`download.py` offers a file through OSC 1337, including across SSH.
Afterglow asks where to save the file.

The image and cursor clients show Kitty protocols.
iTerm2 `it2dl` uses the same download format.
See [supported operations and limits](../../docs/COMPATIBILITY.md).

For a minimal color example inside an application-owned screen:

```python
# Set extra-cursor fill to turquoise and text to dark blue.
print("\033[>40;2:80:220:200 q\033[>30;2:12:24:36 q", end="")
# Place a block cursor at row 3, column 11 (coordinates start at 1).
print("\033[>1;2:3:11 q", end="", flush=True)
# Clear positions and restore colors when the application exits.
print("\033[>0;4 q\033[>30;0 q\033[>40;0 q", end="", flush=True)
```

A client can query capabilities with `CSI > SPACE q`.
Use `CSI > 100 SPACE q` for positions and `CSI > 101 SPACE q` for colors.
Read replies from terminal input. Do not treat them as user keystrokes.
The demonstration sends documented operations directly and requires terminal support for those operations.
