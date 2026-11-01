<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Release notes

## Unreleased

This is the first release of Afterglow, a terminal for Linux on Wayland with Vulkan rendering.
Native desktop tests use KDE Plasma and KWin. Other compositors and screen readers are not individually certified.

### Terminal emulation

- VT100 to VT500 control sequences, with conformance levels and the `afterglow` terminfo entry.
- Scrollback with reflow on resize. Rows that still fit keep their cells, styles and graphemes.
- Unicode graphemes, wide characters, color emoji, font shaping and optional primary-screen bidirectional layout.
- Text attributes, including curly, dotted and dashed underlines, underline colors, overlines and OSC 8 hyperlinks.
- Rectangular editing (DECFRA, DECCRA), character repeat (REP) and cursor shapes (DECSCUSR).
- Status queries: DECRQM, DECRQSS, XTGETTCAP and XTWINOPS size reports.
- Synchronized output (mode 2026), grapheme clustering (mode 2027) and in-band resize reports (mode 2048).
- Color-scheme change reports (mode 2031) and visibility reports (mode 2033).
- Screen checksums (DECRQCRA) when `Terminal.allowScreenReadback` is on. They are off by default, because they let any program read the screen.
- Blinking text (SGR 5) at an interval that `Terminal.textBlinkInterval` sets.

### Keyboard and mouse

- The Kitty keyboard protocol, with a flag stack for each screen, and xterm `modifyOtherKeys`.
- Mouse reporting in X10, normal, button-event and any-event modes, with UTF-8, SGR, urxvt and SGR-pixel encodings.
- Focus reports, bracketed paste and alternate-scroll mode.
- Keypad keys with Num Lock off, which the Kitty protocol reports as keypad keys, and the function keys F13 to F35.
- The Kitty mouse-pointer protocol (OSC 22), with separate shape stacks for the main and alternate screens.
- Pastes keep tab, carriage return and line feed, and drop all other control characters.
- A prompt before a paste with a line break reaches a program that does not use bracketed paste.

### Graphics and text sizing

- Kitty graphics: transmission, placement, animation, Unicode placeholders and relative placements with parent chains.
- Kitty images sent as a file, a temporary file or shared memory, unless `Terminal.graphicsFileTransfer` is `deny`.
- An image memory limit for the main and the alternate screen of each pane (`Graphics.quota`, 320 MiB each by default). The oldest images go first.
- Sixel images.
- iTerm2 inline images and file downloads (OSC 1337), with a native Save dialog.
- Kitty text sizing (OSC 66).
- The Kitty multiple-cursors protocol, with mixed block and beam layering.

### Colors and themes

- Palette and dynamic colors (OSC 4, 10, 11, 12, 104 and 110 to 112).
- The Kitty color-control protocol (OSC 21), including transparent background colors and the color stack.
- Themes in Afterglow JSON, Ghostty and Kitty formats, which reload when the file changes.
- 33 bundled themes: Afterglow, Ayu, Catppuccin, Dracula, Everforest, GitHub, Gruvbox, Kanagawa, Monokai, Nord, One, Rosé Pine, Solarized and Tokyo Night.
- `light:NAME,dark:NAME` theme pairs that follow the desktop color scheme.
- Search match and visual bell colors, set in `Colors` or by a theme.
- `show_last_output` opens the last command's output in a pager, up to `Terminal.lastOutputLimit` (1 MiB by default).
- Colors as SVG names or in hex. A color in eight-digit hex is refused, because it does not say where the alpha is.

### Shell integration

- Automatic integration for Bash 4.4 or later, Zsh and Fish 4 or later.
- Prompt, command and exit-status markers (OSC 133 and OSC 633).
- The working directory from OSC 7, OSC 9;9, OSC 633 and iTerm2 `CurrentDir`. New tabs and splits start there when it is on this host.
- A pane closes when its shell exits, also while a background job holds the terminal.
- Command navigation, last-command output in a pager, and command status in tab and pane labels.
- Terminal contexts (OSC 3008) for remote and elevated sessions, and program status reports (OSC 7501).
- `afterglow ssh`, which integrated shells use for `ssh`. It labels the pane with the remote host and installs the `afterglow` terminfo entry on the host when needed.

### Clipboard, notifications and files

- Clipboard access through OSC 52 and the Kitty clipboard protocol (OSC 5522).
- Clipboard reads ask first by default, and the prompt starts on Deny. `Terminal.clipboardWrite` allows, asks for or denies writes.
- Desktop notifications from OSC 9, OSC 777 and the Kitty notification protocol (OSC 99).
- A notification limit for each pane (`Notifications.rateLimit`, 20 in 10 seconds by default). Notices from Afterglow itself do not count.
- Kitty file transfers (OSC 5113) with approval, directories, links, compression and delta updates.
- Kitty drag and drop, including remote directory data and thumbnails.
- Local file drops, quoted for the shell and sent as bracketed paste.

### Windows, tabs and panes

- Tabs and split panes, with pane zoom, resize, swap, rotation and directional focus.
- Ctrl+Shift+Alt+= equalizes the splits of a tab. Ctrl++ and Ctrl+= make the font larger.
- Animated splits whose motion takes any Phosphor curve, such as a spring or a cubic Bézier.
- Phosphor curves for the cursor, scrolling, the visual bell and the quick terminal (`Animation.cursorCurve`, `scrollCurve`, `bellCurve` and `QuickTerminal.curve`).
- A pane overview that shows the panes of every tab as live 3D cards to choose from, drawn by a replaceable shader pack.
- Animated tab switches, drawn by a replaceable shader pack: a turning cube, a page flip, a shatter or a slide.
- Panes that swing in on a hinge or switch on like a picture tube as they split. They fall away or switch off as they close.
- A portal across a pane whose shell moves to another host, user or container, and a colored frame while it is away.
- Pane and tab moves between tabs and windows. A drag outside the tab bar detaches a tab into a new window.
- Saved projects that store a window's tabs, splits, directories and launch arguments.
- A quick terminal on a Wayland layer surface, toggled by a global shortcut. It slides from the top or bottom edge of a chosen screen.
- Optional background blur on KWin, window transparency and an overlay history scrollbar that fades when idle.

### Search, selection and input

- Search with PCRE2 regular expressions, across soft-wrapped lines.
- Keyboard selection in character, line and rectangle modes.
- Text hints that copy, open or run commands on regular-expression matches, and URL hints, labeled from `Hints.alphabet`.
- Links open directly when their scheme is in `Links.schemes` (http, https, mailto, ftp and file by default). Other links ask first.
- Keybindings with multi-key sequences, a leader key and named key tables.
- Bindings that match Shift+Tab and shifted symbols, and that use the Latin layout for letters on other keyboard layouts.
- A command palette for actions, projects and script actions.

### Shader effects

- Phosphor shader packs in five chains: background, text and cursor layers, panes and the window.
- A Phosphor shader-family contract: Phosphor's base uniforms plus terminal inputs such as `uCursorRect`, `uSignal` and `uPalette`.
- Packs that read the pointer (`iMouse` or `uWindowMouse`) draw again when it moves.
- Packs that react only to event ages (`cursorAge()`, `focusAge()`, `signalAge()`) stop drawing once their animation has settled. A pack sets the time with `settleSeconds`.
- `panePreviousContext()` in the shader contract, so that a pack can blend from where a pane's shell ran before.
- Up to eight buffer passes, four image parameters, auxiliary depth, vertex stages and instanced OBJ or generated meshes.
- Object hierarchies for mesh passes, with transforms that follow parameters, scripts and transitions.
- System audio input through CAVA, with its framerate, smoothing and frequency range in `Audio` settings.
- Scrollback-driven inputs and parameter transitions with Phosphor curves.
- Time helpers `timeAngle` and `timeCycle` that keep full precision however long the window runs, and `timeSeconds` for the full clock.
- Bundled packs: Bloom, Cursor Trail, Depth Trails, Feedback Glow, Mesh Reactor, Orrery, Pane Deck, Pane Effects, Phosphor Depths and Tab Switch.
- Pane rectangles, every tab's pane pixels and overview state for window shaders, through `terminal_panes.glsl`.
- The Phosphor and Glasshouse example scenes.

### Scripting and control

- Luau scripts in a sandbox with time and memory limits, which reload when they change.
- Script packages with manifests, configuration, assets and saved state with migrations.
- Script packages that ship their own shader packs, suggest shader settings and name pulse effects.
- Script effects for panes, tabs, windows, shader parameters and presets, with results and tasks.
- `afterglow.await`, `await_all` and `sleep`, so a handler waits for results in place instead of in callbacks.
- Script key modes that take the window's keys. Ctrl+Shift+Escape (`end_script_key_mode`) always ends one.
- A script key mode that starts while the pane overview is open ends when the overview closes.
- `new_tab(pane)` starts in the directory of that pane and opens its tab after the tab of that pane.
- Script control of the pane overview, pane pulses, `on_tab_switch`, and split origins in `on_pane_created`.
- External control through `afterglow ctl` and D-Bus, with access settings.
- A launch that arrives while the first Afterglow process starts waits until that process is ready, then opens its window there.

### Performance

- Afterglow draws only when content changes. A static window submits no frames.
- All windows of one process share one GPU device, one glyph atlas for each display scale and one shader library.
- Layer textures, text history and shader buffers exist only while a configured shader needs them.
- Resize reuses history rows that still fit on one line, without rebuilding their cells.
- Search captures text in small parts and matches it in a worker thread.

Measured medians of RelWithDebInfo builds on an Intel Core i7-9700K and NVIDIA GeForce RTX 2080, in a nested KWin session.
The resize row comes from a separate run of the resize workload.

| Measurement | 1080p | 4K |
| --- | ---: | ---: |
| Idle CPU (% of one core) | 0.09 | 0.08 |
| Key-to-frame response, p50 / p95 (ms) | 1.52 / 5.52 | 2.31 / 4.31 |
| Terminal GPU time while scrolling, p95 (ms) | 0.13 | 0.40 |
| Frame service while resizing with 100,000 history rows, p95 (ms) | 22.85 | 24.58 |
| Memory for 100,000 history rows (MiB) | 86 | 90 |

Script callbacks take a median of about 5 µs with one pane and 21 µs with 32 panes.
See [performance measurements](docs/PERFORMANCE.md) for workloads, conditions and limits.

### Desktop integration

- Terminal text, cursor, selection and geometry for screen readers through Qt and AT-SPI.
- Input-method support with surrounding text for the cursor row.
- Tabs, menus and prompts in the desktop Qt style.
- Settings in `~/.config/afterglow/config.json` that reload when the file changes, and `-o` overrides for one launch.
- With no settings file, every setting has its default. The first change of a setting creates the file, which holds only the values that differ from the defaults. Only its owner can read the file.
- Problems in the settings file, such as an unknown key or a value that is not a color, show as a notice.
- A settings file that does not parse is never replaced. The current settings stay until it is correct.
- The desktop's fixed-width font and size by default, with the `calt` and `liga` features. `Font.sizeStep` sets the font size step.

### Documentation

- [Documentation index](docs/README.md).
- [Supported protocols and limits](docs/COMPATIBILITY.md).
- [Desktop integration](docs/DESKTOP.md) for accessibility and input-method limits.
- [Bidirectional text](docs/BIDI.md) for the application-grid policy.
- [Testing](docs/TESTING.md) for regression and native desktop test procedures.
