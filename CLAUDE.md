<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Afterglow

Afterglow is a Linux terminal for Wayland: Qt 6, C++20, a Vulkan renderer through QRhi, tabs and splits, shader
packs that react to the terminal, shell integration and Luau scripts. It is not released yet.

`docs/README.md` indexes the user documents. `docs/SHADER-API.md`, `docs/SCRIPTING.md` and `docs/PACKAGES.md` are
contracts for pack and script authors: change them with the code. This file holds the rules that sessions get
wrong, and points to the skills that hold the procedures.

## Rules

- **Commit and push finished work to `main` without asking.** "Finished" means built with no warnings, tested, and
  verified the way the change needs (see below). Stage files by name.
- **Never install and never use `sudo`.** No install target and no system packages. Tell the user what you need
  and why. The one exception: `cmake --install` into a temporary directory (`--prefix` or `DESTDIR` under `/tmp`),
  to check the install layout.
- **PlasmaZones changes only through its own pull request.** The build compiles the Phosphor libraries from
  `../PlasmaZones/phosphor-libs`. Never edit that checkout from an Afterglow session. When Afterglow needs a Phosphor
  change, say what and why, and let the user decide.
- **Never touch the user's running Afterglow.** This session usually runs inside one of its panes
  (`AFTERGLOW_PANE_ID` is set). `afterglow ctl` without a private bus acts on the user's own terminal, and
  `pkill afterglow` closes their windows. Test in a nested KWin with its own D-Bus (`ag-verify-live`).
- **A build is not a test.** Check a visible change with `afterglow_renderpng` or the native window tests, at the
  edge cases (one pane, two panes, progress 0 and 1, the wrap point of a loop). The commit message says what you ran.
  A green suite has missed a transparent shader, a flicker at a wrap seam and an animation that never stopped.
- **Find the mechanism.** A fix explains why the bug happens. Do not tune a delay, add a retry or special-case one
  input until the symptom goes away.
- **Make it configurable.** A value that a user could want to change (a duration, a curve, a color, a limit, a
  pack) is a setting with a default, not a constant in the code (`ag-add-setting`).
- **Unreleased: no migrations.** Rename a key or change a format directly. Write no compatibility code for old
  configs or old script APIs. `CHANGELOG.md` describes what the first release supports. It is not a log of changes
  and has no migration notes.
- **Licenses.** Every file that can hold a comment starts with `SPDX-FileCopyrightText: 2026 fuddlesworth` and an
  `SPDX-License-Identifier`. The application, its tests and tools are `GPL-3.0-or-later`. Tools that use only
  `libs/afterglow-vt` (the VT benchmark, `tools/conformance`, `tools/fuzz`, `tools/gen_unicode_tables.py`) are
  `LGPL-2.1-or-later`. `libs/**` is `LGPL-2.1-or-later`, tests included. New shader packs are `LGPL-2.1-or-later`.
  Feedback Glow, Depth Trails and Mesh Reactor are `GPL-3.0-or-later`: keep the header that a file has, and do not
  relicense it. `REUSE.toml` licenses files that cannot hold a header (JSON, images).
- **Ask only about real decisions.** When the next step is clear, do it.

## Code

- Namespaces: `Afterglow` (the application), `AfterglowVt`, `AfterglowRender`, `AfterglowPty`.
- `libs/afterglow-vt` and `libs/afterglow-pty` use no Qt. Keep them plain C++20.
- `QT_NO_KEYWORDS` is on everywhere: `Q_EMIT`, `Q_SIGNALS`, `Q_SLOTS`. `QT_NO_CAST_FROM_ASCII` is on for the
  application and the renderer: `QStringLiteral` for constants, `QLatin1String` for comparisons and JSON keys.
- Members are `m_camelCase`. Emit a signal only when the value changed.
- Threads: the terminal callbacks (`ITerminalHost`) run on the PTY reader thread while `Pane::m_mutex` is held.
  They reach the GUI thread only through queued calls. A new callback follows the same contract.
- Logging uses categories named `afterglow.<area>`.
- Match the surrounding code: its comment density, naming and idiom. A comment says why, not what.

## Text

- Documents follow `docs/WRITING.md`: controlled English after ASD-STE100, short sentences, one instruction in each
  step, tables for fields and limits. Keep the root README short. No audit reports, plans or session summaries in
  the repository.
- User-facing strings (setting descriptions, pack metadata, UI text, `CHANGELOG.md`) are plain prose: no em dash
  between clauses, no semicolon between sentences, no spaced hyphen as a dash, no rule-of-three flourishes. Code
  comments are exempt.
- Commit subject: an imperative sentence in sentence case, with no prefix and no full stop ("Animate tab switches
  with a shader pack"). The body says what changed for the user, why, and how you verified it. Check each claim in
  the message against the staged diff. End with the `Co-Authored-By` line that the session instructions give.

## Environment

- The Bash tool runs zsh. Quote globs that may match nothing (`'*.md'`), and never start a word with `=`
  (`echo ====` fails).
- Put temporary files in the session scratchpad, never in the repository.
- `.Codex/` is another tool's local workspace. Never stage it, and never cite its paths in tracked files.
- Never use `pkill`, `killall` or `kill $(pgrep -f ...)`. List with `pgrep -a NAME`, then `kill PID` for a process
  that you started from `build/`.
- The context is small. Read logs with `grep` and `tail`, never in full.

## Build and test

| Task | Command |
| --- | --- |
| Build | `cmake --build build -j6` (zero warnings is the bar) |
| Unit and renderer tests | `ctest --test-dir build --output-on-failure -j6` (about 500, all pass) |
| Native window tests | `python3 .claude/skills/ag-verify-live/headless_native.py` |
| Shader preview | `build/tools/render/afterglow_renderpng --shader PACK - out.png` |
| Configuration reference | `python3 tools/generate-reference.py build/afterglow/afterglow` |

The `ag-build` skill has the details: the build directories, the Phosphor checkout, the generated files.

## Where things are

| Area | Files |
| --- | --- |
| Terminal core | `libs/afterglow-vt/`: `Terminal.cpp` (parser and dispatch), `Screen.cpp` (grid, scrollback, reflow), `Input.cpp` (key and mouse encoding), `Graphics.cpp`, `Sixel.cpp`, `ITerm2.cpp` (images), `Clipboard.cpp`, `Notifications.cpp`, `ShellContext.cpp` (OSC 133 and host context), `Unicode.cpp` with the generated `UnicodeTables.inc` |
| PTY | `libs/afterglow-pty/src/Pty.cpp`: spawn, the reader thread, bounded input queue |
| Renderer | `libs/afterglow-render/`: `GridRenderer.cpp` (panes, window composition, the overview and other tabs), `PostChain.cpp` (shader chains, buffers, budgets, the pane and tab images), `ShaderProgram.*` (the uniform mirrors of the GLSL contract), `GlyphCache.cpp`, `FontSet.cpp`, `Bidi.cpp`, `GpuDevice.cpp` |
| Window | `afterglow/src/TerminalWindow.cpp` (input routing, render loop, panes and tabs), `TerminalWindowLayout.cpp`, `TerminalWindowOverview.cpp` (overview, tab transitions, pane effects), `TerminalWindowProject.cpp`, `TerminalWindowScript.cpp`, `Pane.cpp`, `SplitTree.cpp`, `KeyMapping.cpp`, `XkbKeyboard.cpp`, `animation/` |
| Chrome and QML | `afterglow/src/chrome/`, `afterglow/qml/` (tab bar, palette, prompts, menus) |
| Configuration | `afterglow/src/config/`: `Settings.*` (schema, defaults, `SettingsStore` over PhosphorConfig), `Keybinds.*`, `KeyModes.*`, `Theme.*`; `data/themes/` |
| Shader library | `afterglow/src/shaders/`: `ShaderLibrary.*` (the pack registry, with package packs), `ShaderControls.*`; packs in `data/shaders/`, the contract in `data/shaders/shared/` |
| Scripting | `afterglow/src/scripting/`: `ScriptHost.*` (Luau VMs, events, effect validation), `ScriptControl.cpp` (effects applied to windows), `ScriptPackage.*`, `ScriptState.*`, `ScriptTasks.*`, the prelude `afterglow.luau` and the declarations `afterglow.d.luau`; examples in `data/examples/scripts/` and `data/examples/packages/` |
| Control | `afterglow/src/control/`: D-Bus control and `afterglow ctl` |
| Shell and desktop | `afterglow/src/shell/` (shell integration, `afterglow ssh`), `data/shell-integration/`, `afterglow/src/desktop/` (accessibility, drag and drop, file transfer), `afterglow/src/audio/` |
| Tests | `libs/*/tests/`, `afterglow/tests/` (GTest, and the Python acceptance runners), `tools/conformance/`, `tools/fuzz/` |
| Tools | `tools/render/` (`afterglow_renderpng`), `tools/bench/`, `tools/generate-reference.py`, `tools/gen_unicode_tables.py` |

## Skills and agents

The skills in `.claude/skills/`:

- `ag-build`: build directories, the Phosphor checkout, warnings, the test suite, generated files.
- `ag-verify-live`: native window tests in a headless nested KWin, and the expected failures there.
- `ag-shader-pack`: pack layout, contract changes and their C++ mirrors, previews with `afterglow_renderpng`.
- `ag-add-setting`: the files that a new setting touches, and the reference regeneration.
- `ag-script-api`: a new script effect, event or helper, from the prelude to the docs.
- `code-audit`: the audit and fix loop, with the `ag-*-reviewer` agents in `.claude/agents/review/`.
- `record-lesson`: where a lesson goes, so that the next session finds it.

The reviewers in `.claude/agents/review/` report findings for one area each. They never build or edit:
`ag-vt-reviewer`, `ag-platform-reviewer`, `ag-render-reviewer`, `ag-shader-reviewer`, `ag-window-reviewer`,
`ag-scripting-reviewer`, `ag-config-reviewer` and `ag-build-reviewer`.

## Lessons that cost a session

- A persistent shader buffer over the 256 MiB budget was transparent with no message, so Orrery drew nothing. Now
  it renders smaller and logs a warning. When a pack draws nothing, check the budget log first.
- An effect that animates only while visible must still stop scheduling frames when it has nothing to move. Pane
  effects kept the window at full frame rate while a pane was remote, until `active` and `moving` became separate.
- A motion that loops (the overview ring) needs a continuous position. Wrapping the target made the highlight jump
  back, and two cards flickered at the seam.
- `newTab` did not start the tab transition, because the code read `m_splits` before the tab had one. Check a new
  path through every entry point (keys, menu, script, D-Bus), not only the one you tested.
- Package shader packs are keyed by a UUID derived from their id. A lookup by id must go through `packagedRef`.
- Per-pixel loops over every card cost 10 ms at 4K with 27 panes. Find the nearest candidates first, then shade
  only those. Measure with `afterglow_renderpng --timings --pixels 3840x2160`.
