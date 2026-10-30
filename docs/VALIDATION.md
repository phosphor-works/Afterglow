<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Delivery validation

## Documentation and compiler diagnostics, 30 October 2026

The README now lists main features and links to focused documents.
Configuration descriptions use the runtime schema. Keys, defaults, ranges and bindings remain unchanged.
All 43 Luau helper-reference rows and 93 audit IDs remain available.

| Check | Result |
| --- | --- |
| Clean rebuild before corrections | Seven compiler warnings reproduced. |
| Rebuild after corrections | Passed without compiler warnings. |
| Full CTest suite | 412/412 passed. |
| Native windows with generic Qt platform theme | 12/12 passed. |
| Documentation | 107 local links resolve. All 12 JSON examples parse. Configuration generation is reproducible. |

The corrections initialize test launch options and add braces around a conditional test assertion.
CMake still reports four intentional Qt private-API dependencies.
See the [Qt rebuild requirement](BUILD.md#qt-private-apis).

The first native run timed out waiting for the drag press while KDE portal services started.
A generic-platform-theme run passed all cases.
The native runner now selects that theme explicitly, as the desktop acceptance runner already does.

## Completion audit, 30 October 2026

The [completion audit](AUDIT.md) covers the repository and delivery plan through V1 and D1, excluding build and installation work.
Validation uses the existing RelWithDebInfo tree, Qt 6.12, KDE/KWin Wayland and NVIDIA RTX 2080 Vulkan.
Packaging and installation remain deferred.

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

The depth-trails run recorded p50 0.85 ms and p95 1.86 ms for render plus readback after warmup.
This short API-example run is not a GPU-only benchmark.
It does not update Phosphor-scene performance measurements.
The [original Phosphor measurements](../data/examples/phosphor/VALIDATION.md) retain their workload and limits.

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
python3 afterglow/tests/native_window_acceptance.py --binary build/afterglow/afterglow_native_tests
python3 afterglow/tests/desktop_acceptance.py build/afterglow/afterglow
python3 tools/conformance/test_headless.py build/tools/conformance/afterglow_headless
python3 tools/generate-reference.py build/afterglow/afterglow
```

Bidirectional layout remains optional with [bounded application-grid fallback](BIDI.md).
IME tests check Qt composition, commit and surrounding-text queries. They do not test external IBus or Fcitx candidate applications.
Accessibility acceptance does not certify screen readers.
Shader auxiliary depth is a sampled color attachment. It does not provide a 3D geometry or depth-test engine.
Display HDR, persistent multiplexing, resumable transfers and cross-platform distribution remain separate product work.

## Original D1 acceptance evidence

Implementation: `4eaf265`.
Validation date: 30 October 2026.
Environment: existing RelWithDebInfo tree, Qt 6.12, KDE/KWin Wayland and NVIDIA RTX 2080 Vulkan.
The change did not include installation or packaging work.

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

The upstream script came from `https://iterm2.com/utilities/it2dl`.
SHA-256: `e19325bb81ca19cd3be22af7e58f1ef9a334ff71e8d71133abea2099f11f26e3`.
The repository does not include this script. Tests do not download it automatically.
Pass an inspected local copy with `--it2dl /path/to/it2dl` to repeat the check.

```sh
ctest --test-dir build --output-on-failure -j4
python3 afterglow/tests/desktop_acceptance.py build/afterglow/afterglow
python3 afterglow/tests/desktop_acceptance.py build/afterglow/afterglow --it2dl /path/to/it2dl
python3 tools/generate-reference.py build/afterglow/afterglow
```

The native test starts its own accessibility registry and uses a generic Qt platform theme.
Save and Cancel remain inside the isolated desktop without portal activation.
The test checks public AT-SPI interfaces. It does not certify Orca or NVDA.
At the original checkpoint, IME candidate interfaces and physical file-manager drag required manual checks.
Bidirectional layout and multiple-cursor colors were not yet implemented.
The completion-audit results above update these limits where implementation is complete.
Resumable transfers remain outside this workflow.
