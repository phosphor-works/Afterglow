<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Compatibility examples

Run these inside Afterglow; they do not modify your configuration.

```sh
python3 data/examples/compatibility/relative-images.py
python3 data/examples/compatibility/multi-edit.py
python3 data/examples/compatibility/download.py /path/to/file.txt
```

The first animates a parent image while its child follows the declared offset. The second edits three ASCII values together using native beam cursors; Escape exits. Both restore the normal screen on exit. The third offers a file through OSC 1337, including across SSH; Afterglow asks where to save it.

The image and cursor clients also demonstrate the corresponding Kitty protocols. The download format is shared with iTerm2's `it2dl`. See [supported operations and limits](../../../docs/COMPATIBILITY.md).
