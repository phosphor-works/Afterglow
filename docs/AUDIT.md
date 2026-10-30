<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Delivery completion audit

Baseline: `cbbd8b8`.
Scope: repository implementation and delivery-plan gaps through V1 and D1, excluding build and installation work.
The baseline contains 298 files.
Review covered root files, application, configuration, scripting, VT, PTY and renderer code.
Four baseline passes preceded corrections. Three confirmation rounds checked changes and their consumers.
Confirmation review also covered new files.

All 93 recorded items are fixed and validated.
Items include feature gaps, protocol requirements, UI acceptance requirements and defects.
Implementation, confirmation reviews and required tests are complete.

| Metric | Result |
| --- | --- |
| Baseline passes | 4 |
| Confirmation rounds | 3 |
| Files in baseline scope | 298 |
| Files fully reviewed | 298/298, plus added files |
| Coverage | 100% of the declared inventory |
| Files changed, including additions/deletions | 151 |
| Recorded items | 93 fixed; 0 open; 0 descoped |
| CRITICAL | 0 found → 0 fixed |
| HIGH | 25 found → 25 fixed |
| MEDIUM | 47 found → 47 fixed |
| LOW | 21 found → 21 fixed |
| NIT | 0 found → 0 fixed |
| Build | PASS |
| CTest | 412/412 PASS |
| Native windows | 12/12 PASS |
| Native AT-SPI/downloads | PASS |
| Verdict | CLEAN within the audited scope |

See [validation evidence and reproduction commands](VALIDATION.md).
Native drag tests use Wayland events delivered by the compositor.
The drag driver uses actual window coordinates and complete readiness data.
The final targeted test passed after that synchronization correction.

## Findings and remediation

Each ID identifies a recorded finding. The resolution column describes the final correction.
References identify the affected implementation where recorded.
The [original report](https://github.com/phosphor-works/Afterglow/blob/0cc16f7/docs/AUDIT.md) retains detailed baseline observations and proposed checks.

### Baseline pass 1

| ID | Severity | Reference | Resolution |
| --- | --- | --- | --- |
| ROOT1 | LOW | `README.md:543` | Replace obsolete package/state claims with the implemented lifecycle. |
| ROOT2 | MEDIUM | `tools/render/renderpng.cpp:211` | Reject nonfinite/out-of-range CLI values and bound framebuffer and image decoding before allocation. |
| ROOT3 | MEDIUM | `tools/conformance/headless.cpp:173` | Strictly parse dimensions and shell-quote responder filenames. |
| ROOT4 | LOW | `tools/fuzz/vtfuzz.cpp:57` | Use independent operation bits so fuzz selection covers all four modes; link PCRE2 in the sanitizer target. |
| ROOT5 | MEDIUM | `data/examples/glasshouse/shaders/glasshouse-glass/effect.frag:109` | Replace reversed smoothstep edges with the complementary ordered expression. |
| ROOT7 | HIGH | Recorded under ROOT7 | Add real TerminalWindow/native compositor tests for composition, focus, paste and drag/drop. |
| R1 | HIGH | `libs/afterglow-render/src/GridRenderer.cpp:100:` | Resolve bounded paragraphs with FriBidi, shape directional runs and share logical/visual mappings with input and accessibility. |
| R2 | HIGH | `libs/afterglow-render/src/GridRenderer.cpp:950:` | Invalidate shaping on font-resource identity changes, including DPR changes. |
| R3 | HIGH | `libs/afterglow-render/src/GridRenderer.cpp:1011:` | Retain bounded atlas pages so overflow cannot invalidate glyphs already queued for the frame. |
| R4 | MEDIUM | `libs/afterglow-render/src/GridRenderer.cpp:1021:` | Apply content scroll displacement to image geometry and trailing placements. |
| R5 | MEDIUM | `libs/afterglow-render/src/FontSet.cpp:35,130:` | Include static weight/slant/width and style in primary/fallback font matching. |
| R6 | MEDIUM | `libs/afterglow-render/src/PostChain.cpp:552-554:`, `Snapshot.h` | Snapshot and pack the actual cursor text and selection colors. |
| R7 | MEDIUM | `data/examples/phosphor/scripts/phosphor.luau:45:` | Recompute heat for the affected surviving tab using closed-pane identity. |
| R8 | LOW | `libs/afterglow-render/src/ShaderProgram.cpp:209:` | Add a runtime failure guard for passthrough compilation. |
| R9 | LOW | `libs/afterglow-render/tests/RenderTest.cpp:66:`, `Snapshot.h:20` | Correct the test helper channel order and exercise non-gray colors. |
| R10 | HIGH | `data/shaders/phosphor-depths/effect.frag:43,48`, `ShaderProgram.cpp:75,78` | Remove duplicate color helpers and compile every bundled/example shader pack. |
| VT01 | HIGH | `libs/afterglow-vt/src/Graphics.cpp:385–395` | Validate the opened payload descriptor and bound offset/length against actual file size. |
| VT02 | HIGH | `Graphics.cpp:261–278,358–364` | Bound pending transfers and file reads before allocation; recover after rejection. |
| VT03 | HIGH | `Graphics.cpp:585` | Cap image, placement and frame metadata independently of decoded pixel storage. |
| VT04 | HIGH | `Graphics.cpp:85–132,1043` | Use wide checked geometry and clip source/destination rectangles before composition. |
| VT05 | HIGH | `libs/afterglow-vt/src/Input.cpp:635–655`, `afterglow/src/TerminalWindow.cpp` | Sanitize paste terminators without synthesizing a new terminator across deletions. |
| VT06 | HIGH | `libs/afterglow-vt/src/Sixel.cpp:146–180` | Bound every sixel coordinate advance, including blank runs. |
| VT07 | HIGH | `libs/afterglow-pty/src/Pty.cpp:125` | Check worker descriptors and flags and unwind startup on failure. |
| VT08 | HIGH | `Pty.cpp:257`, `include/AfterglowPty/Pty.h:78`, `afterglow/src/Pane.cpp:630,638` | Synchronize process identity shared with the I/O thread. |
| VT09 | HIGH | `Pty.cpp` | Bound pending input and per-loop I/O work; observe shutdown and propagate rejection. |
| VT10 | HIGH | `libs/afterglow-vt/src/Clipboard.cpp` | Account cumulative encoded clipboard bytes and bound metadata across partial chunks. |
| VT11 | MEDIUM | `Terminal.cpp:1658,1810` | Centralize protocol and extra-cursor reset state. |
| VT12 | MEDIUM | `Terminal.cpp:364` | Retarget retained semantic marks through reflow rather than retaining stale row identities. |
| VT13 | MEDIUM | `Terminal.cpp:3106` | Join soft wraps consistently in screen text extraction. |
| VT14 | MEDIUM | `Selection.cpp` | Normalize selection endpoints to complete wide glyphs. |
| VT15 | MEDIUM | `ProgramStatus.cpp:171` | Enable native progress reporting only after a valid report. |
| VT16 | MEDIUM | `ProgramStatus.cpp:314–319` | Preserve an insertion index rather than an invalidated vector iterator. |
| VT17 | HIGH | `Graphics.cpp:136–149,349–373` | Validate opened regular descriptors and canonical temporary-file ownership before reads/cleanup. |
| VT18 | MEDIUM | `Terminal.cpp:1562–1586` | Retain and release saved cursor hyperlink references symmetrically. |
| VT19 | LOW | `include/AfterglowVt/Terminal.h:143–160` | Attach callback documentation to the correct declarations. |

### Baseline pass 2

| ID | Severity | Reference | Resolution |
| --- | --- | --- | --- |
| ROOT8 | MEDIUM | `tools/conformance/headless.cpp` | Queue bounded nonblocking PTY replies and poll for writable readiness. |
| ROOT9 | LOW | `tools/conformance/headless.cpp` | Provide a temporary responder snapshot when no explicit dump path is supplied. |
| ROOT10 | MEDIUM | `tools/conformance/esctest.sh` | Require a complete consistent nonempty esctest summary and reject runner exceptions. |
| ROOT11 | LOW | `data/examples/packages/project-navigator/providers.luau` | Floor the bounded discovery depth before passing it to fd. |
| ROOT12 | MEDIUM | `data/shaders/bloom/effect.frag` | Handle threshold 1 as a hard threshold instead of equal-edge smoothstep. |
| VT20 | MEDIUM | `Screen.cpp:109–119,201–224`, `Terminal.cpp:1067` | Retain the complete bounded OSC66 sized-text payload without weakening ordinary grapheme limits. |
| VT21 | MEDIUM | `Terminal.cpp:364–376` | Publish generation/resize notification changes when only pixel metrics change. |
| VT22 | MEDIUM | `Unicode.cpp:26–30` | Preserve ASCII emoji/keycap properties for variation-selector widening. |
| VT23 | MEDIUM | `Terminal.cpp:1018–1057` | Reject overlong and invalid UTF-8 scalars in sized text. |
| VT24 | HIGH | `Terminal.cpp:3326` | Apply separate bounded title, hyperlink and command metadata limits. |
| VT25 | LOW | `Notifications.cpp:18,260–272` | Enforce the combined notification text budget independently of icon data. |
| VT26 | LOW | `ShellContext.cpp:88–104` | Apply the context stack limit to remote-host insertion. |
| VT27 | LOW | `Terminal.cpp:343` | Correct the history-retention comment. |
| A1 | HIGH | `afterglow/src/TerminalWindow.cpp:1135` | Pair emitted panes with render entries so zoomed/empty leaves cannot index past the frame. |
| A2 | MEDIUM | `afterglow/src/Pane.cpp:458` | Preserve preedit graphemes and translate UTF-16 caret positions. |
| A3 | HIGH | `afterglow/src/desktop/Accessibility.cpp:122` | Bridge the chrome accessibility tree to the native window and hide zoomed-away panes. |
| A4 | MEDIUM | `afterglow/src/app/NotificationCenter.cpp:248` | Bind notification expiration to replacement generation. |
| A5 | MEDIUM | `afterglow/src/config/Keybinds.cpp:302` | Encode contiguous UTF-16 literal ranges without splitting surrogate pairs. |
| A6 | MEDIUM | `afterglow/src/TerminalWindow.cpp:1335` | Forward IME queries/commits and map the focused chrome editor rectangle. |
| A7 | MEDIUM | Recorded under A7 | Dispatch keyboard/accessibility scrollbar changes without controller feedback loops. |
| A8 | MEDIUM | `afterglow/src/scripting/ScriptHost.cpp:226`, `z.luau`, `a.luau` | Carry directory precedence through explicit metadata-ID resolution. |
| A9 | LOW | `afterglow/src/scripting/ScriptHost.cpp:197` | Reject live state-directory mutation at runtime. |
| A10 | HIGH | `TerminalWindow.cpp:466–487:`, `ControlProject.cpp:39,82`, `TerminalWindowProject.cpp:35` | Propagate executable startup errors, roll back ordinary creation and guard project recovery paths. |
| A11 | MEDIUM | `TerminalWindow.cpp:2820–2852:` | Track consumed physical key presses through release and forward only to the owning chrome interaction. |
| A12 | MEDIUM | `TerminalWindow.cpp:274,3080:` | Retain the originating pane for paste confirmation and cancel invalid targets. |
| A13 | LOW | `control/Ctl.cpp:236:` | Include PaneMoved in CLI event watching. |
| A14 | LOW | `TerminalWindow.h:243`, `Pane.h:134`, `Control.h` | Remove unused creation/provider helpers and fields; reconcile API comments with their callers. |

### Baseline pass 3

| ID | Severity | Reference | Resolution |
| --- | --- | --- | --- |
| FEATURE1 | HIGH | Complete multiple-cursor color operations30/40/query101, snapshot per-cell color resolution, rendering, examples, interoperability. | Implement extra-cursor color operations and queries, rendering and interactive examples. |
| FEATURE2 | HIGH | Complete Luau pane creation options: all four split directions,size,cwd,environment plus per-job process environment. | Apply cwd, environment, requested split share and all directions consistently, with launch-error rollback. |
| FEATURE3 | MEDIUM | Expose promised inspectable script timings/errors/dropped work counters and bounded semantic/selection text queries; reconcile read-policy promise. | Expose lifecycle timings/errors/drops, bounded selection/semantic-region reads and a script text-read policy. |
| FEATURE4 | HIGH | Complete V1 multi-buffer/per-buffer metadata, repeat/mirror wrap and image parameter live/preset adapter functionality. | Implement eight buffers, four image slots, sampler modes, auxiliary depth and custom vertex hooks with live/preset integration. |
| A15 | HIGH | Recorded under A15 | Use an owner-only checked QTemporaryFile for merged configuration. |
| A16 | MEDIUM | Recorded under A16 | Watch parent directories so external themes recover after deletion/recreation. |
| A17 | LOW | Recorded under A17 | Restore the configuration environment after each fixture. |
| A18 | HIGH | Recorded under A18 | Defer PTY worker startup until pane ownership is installed; stop/join before teardown. |
| ROOT14 | MEDIUM | Recorded under ROOT14 | Capture the sampled scene target and discard obsolete asynchronous results. |
| A19 | MEDIUM | Recorded under A19 | Include DPR in chrome render-target invalidation. |
| A20 | MEDIUM | Recorded under A20 | Validate image dimensions and decoded bounds before retaining notification textures. |
| A21 | MEDIUM | Recorded under A21 | Scope icon identity and cache cleanup to the owning pane. |
| A22 | LOW | Recorded under A22 | Release pane-keyed notification rate state when its owner closes. |
| R11 | MEDIUM | Recorded under R11 | Allow the full scale×width range in sprite cache keys. |

### Confirmation 1

| ID | Severity | Reference | Resolution |
| --- | --- | --- | --- |
| C1 | MEDIUM | Recorded under C1 | Read bounded bytes from the same nonblocking regular descriptor that was validated. |
| C2 | LOW | Recorded under C2 | Anchor script IDs to the absolute beginning/end of the string. |
| C3 | MEDIUM | Recorded under C3 | Count retained decoded images, including transition endpoints, and reject oversized live updates atomically. |
| C4 | MEDIUM | Recorded under C4 | Deliver key release to still-focused chrome controls without leaking it to the PTY. |
| C5 | MEDIUM | Recorded under C5 | Reference the correct visible paste-confirmation object when restoring focus. |
| C6 | MEDIUM | Recorded under C6 | Clear parser overflow state on reset. |
| C7 | MEDIUM | Recorded under C7 | Widen pixel-report and percentage image geometry arithmetic. |
| C8 | MEDIUM | Recorded under C8 | Use matching clearing render-pass descriptors and pipelines for intermediate custom-vertex window passes. |
| C9 | LOW | Recorded under C9 | Retry blocked shader buffers after the requested allocation fits again. |
| C10 | LOW | Recorded under C10 | Guard font/face construction and roll back FreeType/HarfBuzz ownership on exceptions. |
| C11 | MEDIUM | Recorded under C11 | Count UTF-8 bytes and path separators in the file-drop budget. |
| C12 | MEDIUM | Recorded under C12 | Map a tracked wide-tail boundary back to its glyph head during reflow. |
| C13 | MEDIUM | Recorded under C13 | Bind hidden context-menu and paste-panel keyboard focus to visibility. |
| C14 | MEDIUM | Recorded under C14 | Do not permanently cache transient configured-image budget failures. |
| C15 | LOW | Recorded under C15 | Clear theme overrides for explicit Glasshouse worlds; clean temporary JSON through a subshell EXIT trap. |
| C16 | LOW | Recorded under C16 | Regenerate the schema reference and reconcile feature, migration and validation documentation. |

### Confirmation 2

| ID | Severity | Reference | Resolution |
| --- | --- | --- | --- |
| C17 | LOW | Recorded under C17 | Wait for a complete coordinate payload before driving the isolated compositor gesture. |

## Verification boundaries

Build, installation, packaging and package/build CI work remain deferred.
Persistent multiplexing, broad SSH integration, remote package updates, display HDR, cross-platform distribution and resumable transfers remain separate product work.
Bidirectional layout has a bounded primary-screen policy.
Native IME tests do not certify every external candidate interface.

CLEAN means no recorded findings remain open and the documented checks pass.
It does not mean that tests cover every possible defect or external interoperability case.
