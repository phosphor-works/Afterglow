<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Afterglow

Afterglow is a Linux terminal for Wayland: Qt 6, C++20, a Vulkan renderer through QRhi, tabs and splits, shader
packs that react to the terminal, shell integration and Luau scripts. It is not released yet.

`docs/README.md` indexes the user documents. `docs/SHADER-API.md`, `docs/SCRIPTING.md` and `docs/PACKAGES.md` are
contracts for pack and script authors: change them with the code. This file holds the rules that sessions get
wrong, and points to the skills that hold the procedures.

## Rules

- **Land work through a pull request.** Start each change on a branch from `main`. Never push to `main`. Finished
  work is built with no warnings, tested, and verified the way the change needs (see below). Then commit, push the
  branch and open the PR without asking. The user merges pull requests. Stage files by name.
- **Never install and never use `sudo`.** No install target and no system packages. Tell the user what you need
  and why. The one exception: `cmake --install` into a temporary directory (`--prefix` or `DESTDIR` under `/tmp`),
  to check the install layout.
- **PlasmaZones changes only through its own pull request.** The build compiles the Phosphor libraries from
  `../PlasmaZones/phosphor-libs`. Never edit that checkout from an Afterglow session. When Afterglow needs a Phosphor
  change, say what and why, and let the user decide.
- **Never touch the user's running Afterglow.** This session usually runs inside one of its panes
  (`AFTERGLOW_PANE_ID` is set). `afterglow ctl`, `aglow run` and `afterglow run` (also `run --complete`) use the
  pane's `AFTERGLOW_SOCKET`, else the socket of the user's Wayland display, so they act on the user's own terminal.
  `afterglow ssh` calls the pane's `AFTERGLOW_SOCKET` too, and runs `ssh -O` on the user's shared ssh connections.
  A private D-Bus does not change that, and neither does a pseudo-terminal. Run them only against an Afterglow of a
  test, with `AFTERGLOW_PANE_ID` cleared: the `XDG_RUNTIME_DIR` and `AFTERGLOW_SOCKET` of a test (`RunBinaryTest` in
  `CommandSocketTest.cpp`, `RemoteCommandsTest.cpp`), or a nested KWin with its own `WAYLAND_DISPLAY`
  (`nested_desktop.py`). Test in a nested KWin with its own display (`ag-verify-live`). Never run `pkill afterglow`:
  it closes the user's windows too.
  `afterglow mux` (every command but `--help`), `afterglow ctl --session`, `afterglow-mux` and `afterglow --mux-attach
  NAME` (the hidden option that `afterglow mux new` and `attach` run) reach the user's mux daemon and its sessions. Its sockets are in
  `$XDG_RUNTIME_DIR/afterglow-mux` and `$XDG_STATE_HOME/afterglow-mux`, and it starts in the user's systemd unit
  `afterglow-mux.service`. These sockets do not depend on the display, so a nested KWin does not isolate them.
  `afterglow mux new` without `--detach`, `afterglow mux attach` and `--mux-attach` also open a window in the
  Afterglow of the display, and a window of a session ends the session when it closes. Run them only with a private
  `XDG_RUNTIME_DIR` and `XDG_STATE_HOME` under `/tmp` (`mux/tests/TestDaemon.h`), and a nested KWin for a window, and
  kill the sessions that you made. Never stop, restart or mask the user's `afterglow-mux.service`, and never remove the
  files in the user's two `afterglow-mux` directories: that ends the user's sessions, or leaves them without a socket.
  `aglow transfer`, `dnd`, `icat`, `clipboard` and `run`, and the `afterglow` commands of the same names, open
  `/dev/tty`, which is the user's pane. `aglow run` asks it for the socket of an ssh session (OSC 24569), and inside
  tmux it runs `tmux display-message` on the user's tmux. Inside tmux, `aglow icat` sets `allow-passthrough` on the
  user's tmux pane, and `--passthrough=tmux` does that also with `TMUX` unset. Run them only in a pseudo-terminal
  that a test makes (`clipboard_pty.py`, `run_pty.py`, `DragDropTest.cpp`). Only for `aglow icat`, a pipe as its
  output with the options of `icat_decode.py` also works. Unset `TMUX` and never give `--passthrough=tmux`, unless
  the test runs in the private tmux server of `tmux_pty.py`.
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
  `SPDX-License-Identifier`. The application, its tests and tools are `GPL-3.0-or-later`, and so is `mux/` (the
  daemon, its protocol and its tests), and `tools/fuzz/muxfuzz.cpp`, which compiles `mux/`. Tools that use only
  `libs/afterglow-vt`, `libs/afterglow-proto` and `libs/afterglow-image` (the VT benchmark, `tools/conformance`, the
  rest of `tools/fuzz`, `tools/aglow` with its tests, the `aglow` completions in `data/completions`,
  `tools/gen_unicode_tables.py`) are `LGPL-2.1-or-later`. `libs/**` is `LGPL-2.1-or-later`, tests included. New
  shader packs are `LGPL-2.1-or-later`. Feedback Glow, Depth Trails and Mesh Reactor are `GPL-3.0-or-later`: keep
  the header that a file has, and do not relicense it. `REUSE.toml` licenses files that cannot hold a header (JSON,
  images).
- **Ask only about real decisions.** When the next step is clear, do it.

## Code

- Namespaces: `Afterglow` (the application), `AfterglowVt`, `AfterglowProto`, `AfterglowRender`, `AfterglowPty`,
  `AfterglowImage`, `AfterglowMux` (`mux/`: the daemon and the protocol that the application shares with it), and
  `Aglow` (`tools/aglow`, with `Aglow::Cli` and `Aglow::Mime`).
- `libs/afterglow-vt`, `libs/afterglow-proto`, `libs/afterglow-pty`, `libs/afterglow-image` and `mux/` use no Qt.
  Keep them plain C++20.
- Libraries that the build compiles in (zlib-ng, xxHash, libpng, libjpeg-turbo, libwebp) come from archives that
  `cmake/AfterglowDependencies.cmake` pins by URL and SHA-256, and link statically. `aglow` is one static program.
  Do not link a system copy instead.
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
| Build | `systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 cmake --build build -j6` (zero warnings is the bar) |
| Unit and renderer tests | `systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 ctest --test-dir build --output-on-failure -j6` (about 2,000, all pass) |
| Native window tests | `systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 python3 .claude/skills/ag-verify-live/headless_native.py` (about 15 minutes: run it in the background) |
| Shader preview | `build/tools/render/afterglow_renderpng --shader PACK - out.png` |
| Configuration reference | `python3 tools/generate-reference.py build/afterglow/afterglow` |

The memory cap keeps systemd-oomd from killing the user's Afterglow (see the last lesson). Run one build at a time.
The `ag-build` skill has the details: the build directories, the Phosphor checkout, the generated files.

## Where things are

| Area | Files |
| --- | --- |
| Terminal core | `libs/afterglow-vt/`: `Terminal.cpp` (parser and dispatch), `Screen.cpp` (grid, scrollback, reflow), `Input.cpp` (key and mouse encoding), `Graphics.cpp`, `Sixel.cpp`, `ITerm2.cpp` (images), `Notifications.cpp`, `ShellContext.cpp` (OSC 133 and host context), `Unicode.cpp` with the generated `UnicodeTables.inc` |
| Protocol formats | `libs/afterglow-proto/` (no Qt; the terminal side of each format, and the client side that tests and clients of the library use): `Transfer.cpp` and `Delta.cpp` (OSC 5113 file transfer), `DragDrop.cpp` (OSC 72), `Graphics.cpp` (APC G keys and replies), `Clipboard.cpp` (OSC 5522 and OSC 52), `Color.h` and `Color.cpp` (X11 color names and color specs, and OKLCH and CIE LAB with the gamut mapping of CSS Color 4), `Placeholder.h` (Unicode placeholders), `TransferClient.cpp`, `TransferPath.cpp`, `FdDelta.cpp` and `Fd.h` (the transfer client of `aglow transfer`), `DndTree.cpp` and `DndUri.cpp` (the drag-and-drop trees and URI lists of `aglow dnd`), `Version.h`, `Json.cpp` (JSON for the phosphor-ipc client and server), `Ipc.cpp` (the wire client), `IpcServer.cpp` (the wire server of `afterglow-mux`), `Lock.cpp` (the lock file that decides who owns a socket path, for `afterglow-mux` and the Afterglow socket), `Run.cpp` (`aglow run`), `Socket.cpp` (the socket lookup of clients, and the sockets of the mux daemon), `Remote.cpp` (`aglow remote-prune` and the query of `aglow run`) and `RemoteOsc.h` (the OSC 24569 answer that the parser needs), for remote commands, `Base64.h`, `Hex.h`, `Text.h`, `Sha256.cpp`, `Random.cpp`, `Zlib.cpp`; zlib-ng and xxHash from the archives that `cmake/AfterglowDependencies.cmake` pins |
| Image decoders | `libs/afterglow-image/` (no Qt; for `aglow` and `afterglow-mux`, which call it only in a child process with limits): `Decode.cpp` (the format from the signature, the size from the header, the contract in `Decode.h`), `Png.cpp`, `Jpeg.cpp` and `WebP.cpp` (with animations) through libpng, libjpeg-turbo and libwebp, which it builds in, and its own `Gif.cpp` (animations) and `Bmp.cpp` readers; `notices/IJG.txt` (the IJG acknowledgment, installed as `NOTICE.IJG`); tests in `libs/afterglow-image/tests/` (`Image.*`); the decoder of local panes is `afterglow/src/ImageDecode.cpp` (Qt, `WindowImageDecode.*`) |
| PTY | `libs/afterglow-pty/src/Pty.cpp`: spawn, the reader thread, bounded input queue, the pipes of overlays; tests in `libs/afterglow-pty/tests/` (`PtyLib.*`) |
| Renderer | `libs/afterglow-render/`: `GridRenderer.cpp` (panes, window composition, the overview and other tabs), `PostChain.cpp` (shader chains, buffers, budgets, the pane and tab images), `ShaderProgram.*` (the uniform mirrors of the GLSL contract), `GlyphCache.cpp`, `FontSet.cpp`, `Bidi.cpp`, `GpuDevice.cpp` |
| Window | `afterglow/src/TerminalWindow.cpp` (input routing, render loop, panes and tabs), `TerminalWindowLayout.cpp`, `TerminalWindowOverview.cpp` (overview, tab transitions, pane effects), `TerminalWindowProject.cpp`, `TerminalWindowScript.cpp`, `TerminalWindowDock.cpp` (script docks: layout and hits), `DockArea.cpp` (one dock's text and snapshot), `TerminalWindowOverlay.cpp` (overlays over panes: open, fade, end), `PaneOverlay.*` (an overlay's program and terminal), `Pane.cpp`, `SplitTree.cpp`, `KeyMapping.cpp`, `XkbKeyboard.cpp`, `animation/` |
| Chrome and QML | `afterglow/src/chrome/`, `afterglow/qml/` (tab bar, palette, prompts, menus) |
| Configuration | `afterglow/src/config/`: `Settings.*` (schema, defaults, `SettingsStore` over PhosphorConfig), `Keybinds.*`, `KeyModes.*`, `Theme.*`; `data/themes/` |
| Shader library | `afterglow/src/shaders/`: `ShaderLibrary.*` (the pack registry, with package packs), `ShaderControls.*`; packs in `data/shaders/`, the contract in `data/shaders/shared/` |
| Scripting | `afterglow/src/scripting/`: `ScriptHost.*` (Luau VMs, events, effect validation), `ScriptControl.cpp` (effects applied to windows), `ScriptPackage.*`, `ScriptState.*`, `ScriptTasks.*`, `OverlaySpec.*` (the options of an overlay, for scripts and control), the prelude `afterglow.luau` and the declarations `afterglow.d.luau`; examples in `data/examples/scripts/` and `data/examples/packages/` |
| Control | `afterglow/src/control/`: `Control.cpp`, `ControlSocket.cpp` (the `control` target of the phosphor-ipc socket), `CommandSocket.cpp` (the `commands` target: script commands for `aglow run`, and the `remote` target), `RemoteSockets.cpp` (the sockets that `afterglow ssh` forwards for remote commands), `ControlProject.cpp` (projects and directories), `IpcClient.cpp`, `Ctl.cpp` (`afterglow ctl`); the socket and the launch handoff in `afterglow/src/app/InstanceSocket.cpp` and `main.cpp` |
| Mux | `mux/`: `afterglow-mux` (no Qt) with `Daemon.cpp` (the daemon socket and the lifetimes), `DaemonControl.cpp` (the control target of a session, for `afterglow ctl --session`), `Proxy.cpp` (`afterglow-mux proxy`, which carries a connection of `afterglow mux --ssh`), `Session.cpp` (its panes and their layout), `MuxPane.cpp` (a PTY and its terminal, the host calls that wait for the window, and the answers while no window shows the session), `ImageDecoder.cpp` (PNG and iTerm2 images, with `libs/afterglow-image`: the server `afterglow-mux --decode-images`, which the daemon starts once and which forks a process with limits for each image); `Attach.cpp` (the daemon's side of a window that shows a session: its socket, its stream, the frames of its panes and its input); `mux/protocol/MuxStream.*` (the events and the input on that stream), `MuxLayout.*` (the tabs and splits of a session, which the daemon keeps and the window sends), `MuxHost.*` (the calls of a pane's terminal to its host that the window answers: bells, notifications, the clipboard, transfers, drag and drop, downloads, pointer shapes, user variables, shell integration commands and reports), `MuxCodec.h` (the varints of both codecs), `MuxConnect.*` (how a client reaches the daemon: its sockets, or `afterglow-mux proxy` over ssh), `MuxStart.*` (the start of the daemon, Qt-free, for `afterglow mux new` and the proxy); `afterglow/src/mux/MuxClient.cpp` (a window's link to a session, for its session panes in `Pane`), `afterglow/src/control/SessionControl.cpp` (the control calls that the daemon passes to the window of a session); `libs/afterglow-vt/src/StateSync.cpp` (the codec `afterglow.state.v1`: a writer that sends what changed in a terminal, and a reader that applies it to a replica terminal in a window); `mux/protocol/MuxProtocol.*` (the calls and their values, linked by the application too); `IpcServer.cpp` in `libs/afterglow-proto` (the server side of the phosphor-ipc wire format); `afterglow/src/control/Mux.cpp` (`afterglow mux`); `afterglow/src/PaneLaunch.cpp` (the program and environment of a new pane, for windows and sessions); tests in `mux/tests/` (`Mux.*`, with `TestDaemon.h`, which the native tests use too); the user document `docs/MUX.md` |
| Shell and desktop | `afterglow/src/shell/` (shell integration, `afterglow ssh`, and `Aglow.cpp`, which runs `aglow` for `afterglow run`, `transfer`, `dnd`, `icat` and `clipboard`), `data/shell-integration/`, `data/completions/` (the Bash, Zsh and Fish completions of `afterglow` and `aglow`), `afterglow/src/desktop/` (accessibility, drag and drop, file transfer), `afterglow/src/audio/` |
| Tests | `libs/*/tests/`, `afterglow/tests/` (GTest, and the Python acceptance runners), `mux/tests/` (`afterglow_mux_tests`), `tools/aglow/tests/` (`aglow_tests` and the Python tests of `aglow`), `tools/conformance/`, `tools/fuzz/` |
| Tools | `tools/render/` (`afterglow_renderpng`), `tools/bench/`, `tools/generate-reference.py`, `tools/gen_unicode_tables.py`, `tools/vtdiff/`, `tools/sanitizers/`; `tools/aglow/` (`aglow`, the static client of the Kitty protocols and of script commands: no Qt, no Phosphor, only `libs/afterglow-proto` and `libs/afterglow-image`) |

## Skills and agents

The skills in `.claude/skills/`:

- `ag-build`: build directories, the Phosphor checkout, warnings, the test suite, generated files.
- `ag-verify-live`: native window tests in a headless nested KWin, and the expected skips there.
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
  path through every entry point (keys, menu, script, `afterglow ctl`), not only the one you tested.
- Package shader packs are keyed by a UUID derived from their id. A lookup by id must go through `packagedRef`.
- Per-pixel loops over every card cost 10 ms at 4K with 27 panes. Find the nearest candidates first, then shade
  only those. Measure with `afterglow_renderpng --timings --pixels 3840x2160`.
- The native suite segfaulted in 3 of 8 runs, in Qt, while a window built its chrome. A component from
  `loadFromModule(..., Asynchronous)` that was ready before `create(QQmlIncubator&)` took Qt 6.12's C++-type path
  and made no object. A window freed while its own load ran got a late completion into freed memory. Now the
  chrome loads by URL, once for each Application. For a crash in some runs only, read the core with
  `coredumpctl` and write a test that makes it happen every time.
- A session runs in the cgroup of the user's Afterglow. When builds and tests filled memory and swap, systemd-oomd
  killed that whole unit four times, and every Afterglow window of the user closed. Run heavy builds and tests in
  `systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0`, with no more than one build at a time. A failed
  `EXPECT_EQ` on two large strings makes gtest build a line diff that fills the memory: compare them with
  `EXPECT_TRUE(a == b)`.
