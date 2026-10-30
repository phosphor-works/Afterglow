<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Delivery validation

## Completion audit, 30 October 2026

The [completion audit](AUDIT.md) covers the repository and the delivery plan through V1 and non-build D1. Validation uses the existing RelWithDebInfo tree, Qt 6.12, KDE/KWin Wayland and NVIDIA RTX 2080 Vulkan; packaging and installation remain deferred.

| Check | Result |
| --- | --- |
| Full CTest suite | 412/412 passed after the final renderer correction |
| Vulkan renderer | 65/65 passed, including bidi, DPR/cache changes, multipass feedback, images, depth, vertex hooks and clearing uncovered pixels |
| Luau | 68/68 passed, including launch options, semantic-region example, strict IDs, file bounds, diagnostics and reload ownership |
| Shader controls | 15/15 passed, including every bundled/example pack, atomic image updates and recovery after memory pressure |
| VT core | 207/207 passed, including semantic reflow, custom cursor colors, protocol bounds and parser reset |
| Native windows | 12/12 passed together in an isolated KWin desktop, including compositor-delivered QDrag, IME composition/commit, paste focus/ownership, chrome buttons, launch options and script text policy |
| Native AT-SPI/downloads | Public content, caret, selection and geometry checks passed; OSC 1337 Cancel and exact-byte Save passed |
| Sanitizers | Clang ASan/UBSan/libFuzzer: 689,315 inputs in 91 seconds; after the final VT corrections, another 117,975 inputs in 31 seconds; no failure |
| Developer tools | Headless process/reply/responder regressions pass; ten invalid render CLI combinations rejected |
| Reference example | depth-trails compiled and rendered 30 frames at 960×540; output inspected visually |
| Documentation | Runtime configuration reference regenerated; local Markdown links resolve |

The depth-trails run recorded p50 0.85 ms and p95 1.86 ms for render plus readback after warmup. This short API-example run is not a GPU-only benchmark or a new Phosphor-scene performance claim. The [original Phosphor measurements](../data/examples/phosphor/VALIDATION.md) retain their original workload and limits.

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
python3 afterglow/tests/native_window_acceptance.py --binary build/afterglow/afterglow_native_tests
python3 afterglow/tests/desktop_acceptance.py build/afterglow/afterglow
python3 tools/conformance/test_headless.py build/tools/conformance/afterglow_headless
python3 tools/generate-reference.py build/afterglow/afterglow
```

Bidi remains opt-in with [bounded application-grid fallback](BIDI.md). IME tests exercise real Qt composition/commit and surrounding-text queries, not external ibus/fcitx candidate applications. Accessibility acceptance is not screen-reader certification. Shader auxiliary depth is a sampled color attachment, not a 3D geometry/depth-test engine. Display HDR, a persistent mux, full resumable transfers and cross-platform distribution remain separate product work.

## Original D1 acceptance evidence

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

The native test starts its own accessibility registry and uses a generic Qt platform theme so Save/Cancel remain inside the isolated desktop, independent of portal activation. It exercises the public AT-SPI interfaces; it is not an Orca/NVDA certification. At this original checkpoint, actual IME candidate UIs and physical file-manager drag were manual checks, and bidi and multiple-cursor colors were not delivered. The completion-audit results above supersede those limitations where implemented; resumable transfers remain outside this workflow.
