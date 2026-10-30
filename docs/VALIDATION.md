<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# D1 acceptance evidence

Implementation: `4eaf265`. Validation on 30 October 2026 used the existing RelWithDebInfo tree, Qt 6.12, KDE/KWin Wayland and NVIDIA RTX 2080 Vulkan. No installation or packaging changes were made.

| Check | Result |
| --- | --- |
| Full CTest regression suite | 353/353 passed, including actual Vulkan pixel tests |
| Relative graphics | Parent movement/deletion, cycle/missing-parent/depth errors, virtual placeholder movement and transmit-and-place error replies pass |
| Multiple cursors | Capabilities, points/rectangles, clipping and lifecycle pass; mixed block/beam pixels pass with the main cursor hidden and with cursor shaders |
| Unicode placeholder rendering | Pixel regression confirms placeholder slices survive snapshot capture |
| Text and file drop | UTF-16/grapheme mapping, soft wrap, scrolled viewport/live-row separation and path quoting/rejection tests pass |
| Native AT-SPI | Viewport text, caret, selection add/read/remove, character extents and hit testing pass on a private nested desktop |
| Native downloads | Explicit Save writes exact bytes; Cancel completes without a destination write |
| Upstream client | Native save/cancel also passes with the unmodified upstream iTerm2 `it2dl` script |
| References and examples | Configuration generated from runtime schema; local Markdown links resolve; new Python examples compile |

The upstream script was retrieved from `https://iterm2.com/utilities/it2dl`; SHA-256 `e19325bb81ca19cd3be22af7e58f1ef9a334ff71e8d71133abea2099f11f26e3`. It is not vendored or fetched automatically by tests. Pass a locally inspected copy with `--it2dl /path/to/it2dl` to repeat that interoperability check.

```sh
ctest --test-dir build --output-on-failure -j4
python3 afterglow/tests/desktop_acceptance.py build/afterglow/afterglow
python3 afterglow/tests/desktop_acceptance.py build/afterglow/afterglow --it2dl /path/to/it2dl
python3 tools/generate-reference.py build/afterglow/afterglow
```

The native test starts its own accessibility registry and uses a generic Qt platform theme so Save/Cancel remain inside the isolated desktop, independent of portal activation. It exercises the public AT-SPI interfaces; it is not an Orca/NVDA certification. Actual IME candidate UIs and a physical file-manager drag remain manual desktop checks; automated coverage verifies their text/quoting primitives. Full bidi layout, multiple-cursor custom colors and resumable transfers are not claimed.
