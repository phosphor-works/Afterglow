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

### Keyboard and mouse

- The Kitty keyboard protocol, with a flag stack for each screen, and xterm `modifyOtherKeys`.
- Mouse reporting in X10, normal, button-event and any-event modes, with UTF-8, SGR, urxvt and SGR-pixel encodings.
- Focus reports, bracketed paste and alternate-scroll mode.
- The Kitty mouse-pointer protocol (OSC 22), with separate shape stacks for the main and alternate screens.

### Graphics and text sizing

- Kitty graphics: transmission, placement, animation, Unicode placeholders and relative placements with parent chains.
- Sixel images.
- iTerm2 inline images and file downloads (OSC 1337), with a native Save dialog.
- Kitty text sizing (OSC 66).
- The Kitty multiple-cursors protocol, with mixed block and beam layering.

### Colors and themes

- Palette and dynamic colors (OSC 4, 10, 11, 12, 104 and 110 to 112).
- The Kitty color-control protocol (OSC 21), including transparent background colors and the color stack.
- Themes in Afterglow JSON, Ghostty and kitty formats, which reload when the file changes.
- 33 bundled themes: Afterglow, Ayu, Catppuccin, Dracula, Everforest, GitHub, Gruvbox, Kanagawa, Monokai, Nord, One, Rosé Pine, Solarized and Tokyo Night.
- `light:NAME,dark:NAME` theme pairs that follow the desktop color scheme.

### Shell integration

- Automatic integration for Bash 4.4 or later, Zsh and Fish 4 or later.
- Prompt, command and exit-status markers (OSC 133 and OSC 633), and the working directory (OSC 7).
- Command navigation, last-command output in a pager, and command status in tab and pane labels.
- Terminal contexts (OSC 3008) for remote and elevated sessions, and program status reports (OSC 7501).

### Clipboard, notifications and files

- Clipboard access through OSC 52 and the Kitty clipboard protocol (OSC 5522), with confirmation prompts.
- Desktop notifications from OSC 9, OSC 777 and the Kitty notification protocol (OSC 99).
- Kitty file transfers (OSC 5113) with approval, directories, links, compression and delta updates.
- Kitty drag and drop, including remote directory data and thumbnails.
- Local file drops, quoted for the shell and sent as bracketed paste.

### Windows, tabs and panes

- Tabs and split panes, with pane zoom, resize, swap, rotation and directional focus.
- Animated splits whose motion takes any Phosphor curve, such as a spring or a cubic Bézier.
- Pane and tab moves between tabs and windows. A drag outside the tab bar detaches a tab into a new window.
- Saved projects that store a window's tabs, splits, directories and launch arguments.
- A quick terminal on a Wayland layer surface, toggled by a global shortcut.
- Optional background blur on KWin, window transparency and an overlay history scrollbar that fades when idle.

### Search, selection and input

- Search with PCRE2 regular expressions, across soft-wrapped lines.
- Keyboard selection in character, line and rectangle modes.
- Text hints that copy, open or run commands on regular-expression matches, and URL hints.
- Keybindings with multi-key sequences, a leader key and named key tables.
- A command palette for actions, projects and script actions.

### Shader effects

- Phosphor shader packs in five chains: background, text and cursor layers, panes and the window.
- A Phosphor shader-family contract: Phosphor's base uniforms plus terminal inputs such as `uCursorRect`, `uSignal` and `uPalette`.
- Up to eight buffer passes, four image parameters, auxiliary depth, vertex stages and instanced OBJ or generated meshes.
- System audio input through CAVA, scrollback-driven inputs and parameter transitions with Phosphor curves.
- Bundled packs: Bloom, Cursor Trail, Depth Trails, Feedback Glow, Mesh Reactor and Phosphor Depths.
- The Phosphor and Glasshouse example scenes.

### Scripting and control

- Luau scripts in a sandbox with time and memory limits, which reload when they change.
- Script packages with manifests, configuration, assets and saved state with migrations.
- Script effects for panes, tabs, windows, shader parameters and presets, with results and tasks.
- External control through `afterglow ctl` and D-Bus, with access settings.

### Performance

- Afterglow draws only when content changes. A static window submits no frames.
- All windows of one process share one GPU device, one glyph atlas for each display scale and one shader library.
- Layer textures, text history and shader buffers exist only while a configured shader needs them.
- Resize reuses history rows that still fit on one line, without rebuilding their cells.
- Search captures text in small parts and matches it in a worker thread.

Measured medians on an Intel Core i7-9700K and NVIDIA GeForce RTX 2080, in a nested KWin session:

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

## Documentation

- [Documentation index](docs/README.md).
- [Supported protocols and limits](docs/COMPATIBILITY.md).
- [Desktop integration](docs/DESKTOP.md) for accessibility and input-method limits.
- [Bidirectional text](docs/BIDI.md) for the application-grid policy.
- [Testing](docs/TESTING.md) for regression and native desktop test procedures.
