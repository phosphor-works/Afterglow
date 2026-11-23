<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Release notes

## Unreleased

This is the first release of Afterglow, a terminal for Linux on Wayland with Vulkan rendering.
Native desktop tests use KDE Plasma and KWin. Other compositors and screen readers are not individually certified.

### Terminal emulation

- VT100 to VT500 control sequences, with conformance levels and the `afterglow` terminfo entry.
- Scrollback with reflow on resize. Rows that still fit keep their cells, styles and graphemes.
- Unicode graphemes, wide characters, color emoji, font shaping and optional bidirectional layout of the main screen.
- Character widths from Unicode 18.0. As in Kitty, VS15 shows a wide emoji as text in one cell, and VS16 shows a text emoji in two cells.
- A grapheme cluster takes the cells that glibc's `wcwidth` gives its characters, within two cells. Shells and editors that measure text with it then put the cursor in the right place. A spacing mark, a consonant after a virama (Hindi क्र) and Thai SARA AM make a one-cell cluster two cells wide. A prepended mark such as U+0600 ARABIC NUMBER SIGN takes a cell.
- Afterglow draws box drawing, block elements, Braille and the Powerline separators to fill the cell, with no font. It draws the block shapes of Symbols for Legacy Computing (sextants, octants, smooth mosaics, diagonals) the same way.
- Text attributes, including curly, dotted and dashed underlines, underline colors, overlines and OSC 8 hyperlinks.
- Rectangular editing (DECFRA, DECCRA), character repeat (REP) and cursor shapes (DECSCUSR).
- Kitty's ED 22, which moves the lines of the screen into the scrollback before it erases the screen.
- Kitty's unscroll (`CSI n + T`), which brings lines back from the scrollback, and Kitty's SGR 221 and 222, which turn off bold or faint alone.
- DECCARA with every SGR code, as Kitty extends it, and XTSAVE and XTRESTORE without modes for the modes that have no side effects.
- Status queries: DECRQM, DECRQSS, XTGETTCAP and XTWINOPS size reports.
- Synchronized output (mode 2026), grapheme clustering (mode 2027) and in-band resize reports (mode 2048).
- Color-scheme change reports (mode 2031) and visibility reports (mode 2033).
- Screen checksums (DECRQCRA) when `Terminal.allowScreenReadback` is on. They are off by default, because they let any program read the screen.
- Blinking text (SGR 5) at an interval that `Terminal.textBlinkInterval` sets.

### Keyboard and mouse

- The Kitty keyboard protocol, with a flag stack for each screen, alternate keys with the base layout key, and the level and media keys.
- xterm `modifyOtherKeys`.
- Mouse reporting in X10, normal, button-event and any-event modes, with UTF-8, SGR, urxvt and SGR-pixel encodings. With SGR pixels, a program learns when the pointer leaves its pane, as in Kitty.
- Focus reports, bracketed paste and alternate-scroll mode.
- `Keybinds.remapModifiers`, which makes one modifier act as another in Afterglow, as `remap_modifiers` does in Kitty. Bindings, the link modifier, mouse selection and programs get the modifier that a key becomes.
- Keypad keys with Num Lock off, which the Kitty protocol reports as keypad keys, and the function keys F13 to F35.
- The Kitty mouse-pointer protocol (OSC 22), with separate shape stacks for the main and alternate screens.
- Pastes keep tab, send each line end as a carriage return, and drop all other control characters.
- Afterglow asks before a paste or a text drop with a line break reaches a program that does not use bracketed paste.
- Finger scrolls on a touchpad or a touchscreen go on after the last finger lifts, as in Kitty (`Mouse.momentumScroll`, 0.96 by default). `Mouse.touchScrollMultiplier` changes their distance.
- One finger on a touchscreen clicks, scrolls and selects, as in Kitty. A hold of `Mouse.touchSelectHold` (400 milliseconds by default) before a move selects.
- A mouse wheel scrolls `Terminal.scrollLines` rows for each notch.
- `Mouse.autoScrollInterval` sets the time between the rows that a selection scrolls while you extend it.

### Graphics and text sizing

- Kitty graphics: transmission, placement, animation, Unicode placeholders and relative placements with parent chains.
- Kitty indexed images (`f=8` and `f=16`), which take the colors of the pane and change with them.
- Kitty images sent as a file, a temporary file or shared memory, unless `Terminal.graphicsFileTransfer` is `deny`.
- An image memory limit for the main and the alternate screen of each pane (`Graphics.quota`, 320 MiB each by default). Images without a placement go first: those sent with the transient hint (`N=1`), then the oldest.
- Sixel images.
- The images that programs send decode in processes of their own, in local and in session panes alike. Each process has limits on its memory, its time and the system calls that it can make. This needs `aglow-mux` next to `afterglow`, a kernel with seccomp filters and `pidfd_open`, and a processor that the filter knows. On FreeBSD, `aglow` and `aglow-mux` decode in Capsicum capability mode instead. PNG, APNG, JPEG, GIF, WebP, BMP, TIFF and netPBM images show. An image with an ICC color profile or a cICP color space shows in sRGB, as in Kitty.
- iTerm2 inline images, which play when they are animated GIF, WebP or APNG images.
- File downloads (OSC 1337) of any size, as `it2dl` sends them. Afterglow asks before the data comes, with the name of the file and its size when the program declares it. The program waits for the answer (`Terminal.downloads`, `Terminal.downloadDirectory`). The dialog moves with its pane to another window. It closes when its pane of a mux session moves to a new session.
- File uploads that a program asks for with OSC 1337 `RequestUpload`, as `it2ul` does. Afterglow asks, then sends the chosen files or folder as iTerm2 does: a tar archive compressed with gzip, in base64 as typed input (`Terminal.uploads`, `Terminal.uploadLimit`). When a pane of a mux session moves to another session, the next window that shows the pane asks again.
- Kitty text sizing (OSC 66).
- The Kitty multiple-cursors protocol, with mixed block and beam layering.

### Colors and themes

- Palette and dynamic colors (OSC 4, 10 to 12, 17, 19, 104, 110 to 112, 117 and 119).
- The Kitty color-control protocol (OSC 21), including transparent background colors and the color stack.
- Themes in Afterglow JSON, Ghostty, Kitty and Konsole color scheme formats, which reload when the file changes. A theme that Ghostty, Kitty or Konsole installed works by its name alone, such as Ghostty's Catppuccin Mocha or Konsole's Breeze.
- `plasma:NAME` themes, which take the background, text, selection and cursor colors of the desktop color scheme and follow it while Afterglow runs.
- 33 bundled themes: Afterglow, Ayu, Catppuccin, Dracula, Everforest, GitHub, Gruvbox, Kanagawa, Monokai, Nord, One, Rosé Pine, Solarized and Tokyo Night.
- `light:NAME,dark:NAME` theme pairs that follow the desktop color scheme.
- Search match and visual bell colors, set in `Colors` or by a theme.
- A popup border color, set in `Colors` or by a theme. Without one, it is halfway from the background color to the text color. `Colors.popupDim` darkens the pane around a popup.
- Colors as X11 names, hexadecimal values, `rgb:` and `rgbi:` values, and OKLCH and CIE LAB, such as `oklch(0.7 0.1 200)` and `lab(50 60 40)`. Kitty reads these forms too. `Colors`, themes, `aglow icat --background` and OSC 4, 10 to 12, 17, 19 and 21 take them. A color outside sRGB gets the gamut mapping of CSS Color 4. A color in eight-digit hexadecimal is refused, because it does not say where the alpha is.

### Shell integration

- Automatic integration for Bash 4.4 or later, Zsh and Fish, with Kitty's switches in `Terminal.shellIntegration` (`no-prompt-mark`, `no-cwd`, `no-cursor`, `no-title`, `no-sudo`, `no-rc`, `no-complete`). It adds a bar cursor at the prompt, and a `TERM` that `sudo` and `run0` can use. In Bash and Zsh, it sets the directory and the command as the title. Fish sets its own titles. A Fish older than 4 gets the prompt marks from Afterglow.
- Prompt, command and exit-status markers (OSC 133 and OSC 633), with Kitty's secondary prompt (`k=s`) and command lines quoted as by `printf %q`. The integration of Bash and Zsh marks the secondary prompt (`PS2`) of a command that goes on, unless the prompt has marks of its own.
- A click on the command line moves the cursor of the shell there, as Kitty does. The shell gets arrow keys, Kitty's special key or the SGR click of `click_events`. `Mouse.clickMovesCursor` turns it off.
- The working directory from OSC 7, OSC 9;9, OSC 633 and iTerm2 `CurrentDir`. New tabs and splits start there when it is on this host.
- A pane closes when its shell exits, also while a background job holds the terminal.
- Command navigation, last-command output in a pager up to `Terminal.pagerOutputLimit` (1 MiB by default), and command status in tab and pane labels.
- Shell contexts (OSC 3008) for remote and elevated sessions, and program status reports (OSC 7501).
- `afterglow ssh`, which integrated shells use for `ssh`. It labels the pane with the remote host and installs the `afterglow` terminfo entry on the host when needed.

### Clipboard, notifications and files

- Clipboard access through OSC 52 and the Kitty clipboard protocol (OSC 5522).
- Clipboard reads ask first by default, and the prompt starts on Deny. `Terminal.clipboardWrite` allows, asks for or denies writes. **Always allow** for a program's password allows its later requests of the same kind, reads or writes.
- Desktop notifications from OSC 9, OSC 777 and the Kitty notification protocol (OSC 99), which `Notifications.enabled` turns on and off. With notifications off, programs that ask learn that the terminal shows none.
- `Notifications.filters`, rules for the notifications that Afterglow does not show, as `filter_notification` in Kitty. A rule matches the title, body, application and type of a notification.
- A notification limit for each pane (`Notifications.rateLimit` in `Notifications.rateWindow`, 20 in 10 seconds by default). Notices from Afterglow itself do not count, and neither do `afterglow ctl notify` and script notifications. The notice at the end of a script command that has no client counts.
- Settings for the shortest time between two bell sounds (`Terminal.bellSoundInterval`) and between two status notifications of a pane (`Notifications.statusInterval`).
- `Control.eventInterval` sets the shortest time between two control events of one kind for a pane.
- `Terminal.pasteReadTimeout` sets how long a paste event lets a program read the clipboard.
- Kitty file transfers (OSC 5113) with approval, directories, links, compression and delta updates. The approval lists the paths and covers only those paths. They are the files that a program reads, or the destinations of the files that it sends. A file goes only where the approval showed, also when a link changes after it. A file that the approval did not list waits for an approval of its own, and the approved files continue meanwhile.
- Kitty drag and drop, including remote directory data and thumbnails.
- `Terminal.dragAnswerWait` sets how long a drag waits for the answer of a program that uses Kitty drag and drop.
- `Terminal.dragTreeLifetime` sets how long Afterglow keeps the files of a drag from another computer after the drop.
- Afterglow tells the source of a drag the action that the drop took (the `o` key of `t=e:x=4`). `aglow dnd` uses it to delete the files of a remote drag only when the drop was a move. A move to another application is reported as a copy, so the source keeps its files.
- Received file transfers keep the read, write and execute bits of a mode. Afterglow drops the set-user-ID, set-group-ID and sticky bits.
- Local file drops, quoted for the shell and sent as bracketed paste.
- Text drops, pasted with the size limit and the question of a paste, unless the program in the pane accepts drops.
- `aglow`, a client for Kitty file transfer, drag and drop, images and the clipboard. It does not need Kitty. It is one static binary, which you can copy to a remote host with a kernel and processor that it supports. `aglow icat` shows PNG, JPEG, GIF, WebP, BMP, TIFF and netPBM images, and plays GIF, WebP and APNG animations. It converts an image with an ICC color profile or a cICP color space to sRGB. When ImageMagick is installed, `aglow icat` also shows AVIF, HEIC, JPEG XL, QOI, SVG, TGA, ICO and CUR images with ImageMagick. ImageMagick also shows two kinds of BMP images that `aglow` cannot read. These are BMP images with JPEG or PNG data, and BMP images with run-length encoding that does not fit their depth. When curl is installed, `aglow icat` shows images at http, https and ftp URLs. `aglow clipboard` makes a PNG of another image on the clipboard when you ask for a PNG. The `transfer`, `dnd`, `icat` and `clipboard` commands of `aglow` take the options of `kitten`, with a few differences that the compatibility document lists.
- `afterglow run`, `afterglow transfer`, `afterglow dnd`, `afterglow icat` and `afterglow clipboard`, which run the `aglow` that is installed with Afterglow.
- `AFTERGLOW_AGLOW_ONLY` builds `aglow` and `aglow-mux` alone, for a remote host or packages of their own. This build needs no Qt. It installs only the two programs, the private libraries of `aglow-mux`, the completions of `aglow` and the license notices. `aglow-mux` needs PCRE2 and phosphor-channel from phosphor-libs, which the configuration downloads. With `AFTERGLOW_BUILD_MUX` off, the build makes `aglow` alone and needs neither. This build also works on FreeBSD 15, with the clang of the base system, so a FreeBSD server can keep sessions for `afterglow mux --ssh`.

### Windows, tabs and panes

- Tabs and split panes, with pane zoom, resize, swap, rotation and directional focus.
- `Window.resizeStepCells` sets the cells that the resize actions move a split edge by.
- Ctrl+Shift+Alt+= equalizes the splits of a tab. Ctrl++ and Ctrl+= make the font larger.
- Animated splits whose motion takes any Phosphor curve, such as a spring or a cubic Bézier.
- Phosphor curves for the cursor, scrolling, the visual bell and the quick terminal (`Animation.cursorCurve`, `scrollCurve`, `bellCurve` and `QuickTerminal.curve`).
- A pane overview that shows each tab as a live picture of its panes to choose from, drawn by a replaceable shader pack. The arrow keys go to the pane in their direction.
- Animated tab switches, drawn by a replaceable shader pack: the new tab slides in over the tab being left, or the two fade.
- Panes that fade in as they split and fade out as they close, with a rim in the Phosphor spectrum that is bright at once and then fades.
- A thin line at the edge of a pane whose shell moves to another host, user or container, in a color for each. The line is bright at the change and stays while the pane is away. `Animation.contextDuration` sets the time of the change.
- Pane and tab moves between tabs and windows. A drag outside the tab bar detaches a tab into a new window.
- Saved projects that store a window's tabs, splits, directories and launch arguments. A project with `"session": true` opens into a mux session with its name. It shows that session again after its window closed, with the commands and titles of its panes. The windows go on responding while the session starts.
- Terminal sessions that keep running without a window, in the `aglow-mux` daemon. `afterglow mux new` starts a session with one pane and shows it in a window. `afterglow mux attach` shows a session in a window, and `afterglow mux detach` closes that window. One window at a time shows a session, and a second one takes it over. The session follows the settings of the window that shows it, also after you edit them. `Mux.updateEnvironment` names the variables that a session of this computer takes from Afterglow each time a window shows it, such as `SSH_AUTH_SOCK`. `Mux.onClose` decides whether closing that window, or its last tab or pane, ends the session. `afterglow mux ls` and `afterglow mux kill` list and end the sessions.
- A new tab or split in the window of a session starts a new pane of the session. The daemon keeps the tabs and splits, with their ratios, focus, zoom and the titles of tabs and panes. The next window shows them again. Closing one pane or tab ends only its programs. A pane or a tab that moves to a new window goes on as a new session in a new window. A pane that moves into the window of another session of the same daemon joins that session, and its program keeps running. `afterglow ctl --session` can also move a pane to a pane of another session.
- The `--ssh DESTINATION` option of the `afterglow mux` commands keeps sessions on another host and shows them in a window here. The commands run `aglow-mux proxy` there over an ssh connection of Afterglow. You sign in once, and ssh needs no port forward. A session there runs your login shell with the environment of your login there. The `afterglow` terminfo entry is installed there as `afterglow ssh` installs it. When the connection breaks, the session keeps running there. `Mux.remoteCommand` names the program that ssh runs there. `Mux.signInTimeout` sets how long Afterglow waits while you sign in. `Terminal.sshControlPersist` sets how long the connection stays open.
- `afterglow ctl --session NAME` acts on a session through the daemon, also while no window shows it. `afterglow ctl` in a session pane acts on its session. The daemon lists panes, sends and reads their text, and types keys. It reads and sets colors and user variables, and signals programs. It splits, opens, focuses, moves and closes panes. It passes the commands that only a window can do to the window that shows the session, `overlay` and `screenshot` too. `afterglow ctl --session NAME watch` prints the events of the session's panes. A session of another host runs no program of this computer through them, and reads and writes none of its projects, scripts, screen or clipboard. `aglow run` and `afterglow run` in a session pane run the commands of the scripts of that window. In a session of another host, they run only the commands that a script marks remote, as in an ssh session of that host. `afterglow ssh` in a session pane gets remote commands too. The daemon keeps the socket that ssh forwards, and the window that shows the session runs the commands.
- The window that shows a session answers the requests of its programs, as for its own panes. These are bells, notifications, the clipboard, file transfers, drag and drop, downloads, uploads, pointer shapes, user variables and the commands of shell integration. The window asks its usual questions first. `Mux.eventQueue` sets how much waits for a slow window.
- While no window shows a session, each pane keeps its bell, its flash and its newest notifications. The next window rings that bell, flashes the pane and shows those notifications, also for the pane that has the focus. `Mux.keptNotifications` sets how many notifications each pane keeps. The daemon keeps no clipboard writes, because a late write would replace what you copied since.
- A session pane shows the marks of shell integration, program status, shell contexts, extra cursors and images. Before a window shows a session, its cells have the size of your font, so images that the programs print then show too. Each image decodes in a process of its own, so a decoder that crashes cannot end your sessions. The daemon decodes PNG and iTerm2 images (PNG, APNG, JPEG, GIF, WebP, BMP, TIFF and netPBM). A selection moves with its text in a region that a program scrolls. The program hears whether you can see the pane (mode 2033).
- A quick terminal on a Wayland layer surface, toggled by a global shortcut. It slides from the top or bottom edge of a chosen screen. It slides away while a question or a file dialog of one of its panes is open. Examples are the questions of a file transfer, a download and an upload. It comes back when the dialog closes.
- Optional background blur on KWin, window transparency and a history scrollbar over the pane that fades when idle.
- `Window.tabBarHeight` sets the height of the tab bar, and `Window.scrollbarWidth` sets the width of the scrollbar.

### Search, selection and input

- Search with PCRE2 regular expressions, across soft-wrapped lines.
- Keyboard selection in character, line and rectangle modes.
- With `Mouse.dragSelection` on (off by default, as in Kitty), selected text drags to another pane or another desktop application.
- The output of a command in the pager. This is the last output, the first output on the screen, or the last output that a prompt jump went to. A Ctrl+Shift+right-click (`Mouse.commandOutputModifier`) opens the output that you click, or selects it (`Mouse.commandOutputClick`). Scripts read the same outputs with `read_text`.
- Text hints that copy, open or run commands on regular-expression matches, and URL hints, labeled from `Hints.alphabet`.
- Links open directly when their scheme is in `Links.schemes` (http, https, mailto, ftp and file by default). Other links ask first.
- File paths in the text, such as `src/main.cpp:12`, are links when the file or directory exists on this computer. A pane that shows another host, such as a pane that runs `ssh`, has no path links. `Links.openFilesWith` opens a file in an editor at the line.
- Keybindings with multi-key sequences, a leader key and named key tables.
- Bindings that match Shift+Tab and shifted symbols, and that use the Latin layout for letters on other keyboard layouts.
- A command palette for actions, projects and script commands.

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
- Bundled packs in the Phosphor design language: Phosphor Overview, Phosphor Tabs and Phosphor Panes, which are the defaults of their event packs, and Phosphor Depths.
- Bundled neutral packs, which take their colors from the theme: Plain Overview, Plain Tabs and Plain Panes, with the motion of the Phosphor packs and no gradient or glow, Overlay Fade, Bloom and Cursor Trail.
- Example packs in the `examples/shaders` directory of the source repository: CRT Curve, Depth Trails, Feedback Glow, Mesh Reactor, Orrery, Pane Deck, Pane Effects, Tab Switch and Tilt.
- Packs that move the content can define a pointer map (`pPointerMap`). Clicks, selections and links then reach the cell that the pack draws under the pointer.
- Pane rectangles, every tab's pane pixels and overview state for window shaders, through `terminal_panes.glsl`.
- The Phosphor and Glasshouse example scenes, in the `examples` directory of the source repository.

### Scripting and control

- Luau scripts in a sandbox with time and memory limits, which reload when they change. The time limits count the CPU time of the script, so a busy computer does not make a script fail.
- A CPU budget for each script (`Scripts.cpuBudget`, 250 ms of CPU time in each second by default). Past it, Afterglow drops the events of the script, and delays its results, timers, keys and command runs. Each second in which a call of the script waits counts. After 5 such seconds, each less than 3 seconds after the one before, Afterglow disables the script. Dropped events do not count.
- Script packages with manifests, configuration, assets and saved state with migrations.
- Script packages that ship their own shader packs, suggest shader settings and name pulse effects.
- Script effects for panes, tabs, windows, shader parameters and presets, with results and tasks.
- `afterglow.await`, `await_all` and `sleep`, so a handler waits for results in place instead of in callbacks.
- Docked areas: a script puts lines of styled text at an edge of a window, and the panes shrink to fit. An area can take the place of the tab bar. An area can also go at an edge of one pane. It then moves with the pane, also to another tab or window, shows on its overview card and closes with it. It also closes when a pane of a mux session moves to a new session. Its script gets clicks, drags, the item under the pointer, the mouse wheel and the cells that the area has.
- Scripts can open the menu of a tab and move a tab to a position in its window. A pane's actions that close, rename, move or detach its tab do not show that tab first.
- The tab line example package draws the tab bar in the terminal font. Its tabs show a hover state, close on a middle click and open their menu on a right click. A `+` button opens a new tab, the wheel moves through the tabs, and a drag moves a tab.
- Script key modes that take the window's keys. The `end_script_key_mode` binding, Ctrl+Shift+Escape by default, always ends one.
- A script key mode that starts while the pane overview is open ends when the overview closes.
- `new_tab(pane)` starts in the directory of that pane and opens its tab after the tab of that pane.
- Script control of the pane overview, pane pulses, `on_tab_switch`, and split origins in `on_pane_created`.
- External control through `afterglow ctl` and a control socket, with access settings and merged state events.
- `afterglow ctl` drives panes as `kitten @` does. Editor plugins such as smart-splits.nvim can then move between editor splits and panes. `focus --direction` focuses the pane on one side and exits with status 1 at the edge. `resize --direction` moves an edge by a number of cells. `send-key` types keys by name in the program's keyboard mode. `paste` sends text as a paste in the window does, bracketed when the program asks for it. `scroll` scrolls by lines, pages or prompts, or to an end. `set-tab-title` names a tab. `ls` shows what the shell does: at its prompt, at its secondary prompt, running a command, or no shell integration. `ls --json` also tells whether the user typed at the prompt and the exit code of the last command. It also gives the user variables, the foreground process and the neighboring panes.
- Control clients follow a shell through its marks. The `shellChanged` event comes when the shell starts a command, shows a new prompt or waits for the rest of a command. The `textChanged` event comes when a program writes cells of a pane, or the cursor or the view moves, at most once in each `Control.eventInterval`. `afterglow ctl watch` prints it with `--text-changes`. The `mark` call and the `since` scope of `getText` read the output since a command was sent. `shellRegions` gives the shell's prompts, command lines and outputs with their exit codes. A pane that cannot answer for a moment fails with the code `TRY_AGAIN`.
- Agent sessions for programs that drive panes for an agent. An agent session keeps read access for `afterglow ctl`. Its window shows an `agent` badge at the start of the tab bar and `[agent]` before the title (`Mux.agentLabel`, `Mux.agentColor`). Its window detaches when it closes, and the session can end with the client that bound it. `afterglow mux new --agent` makes one, and `afterglow mux ls` marks them. `Control.allowAdopt` lets an agent adopt a session that you started. A script sees it in the `agent` field of the window in its snapshot.
- `afterglow mcp`, an MCP server on standard input and output for agents such as Claude Code. It gives the agent a mux session of its own, which you can watch or take over with `afterglow mux attach`. Its tools open panes, and run shell commands with their exit codes and output. They send keys and pastes to interactive programs, wait for text, read panes and take screenshots. Other tools open a saved project in the agent's session, and save the session as a project with no window. A tool runs a command of your scripts in the window that shows the session, with that window's `Scripts` settings. No tool can add or enable a script. The session ends with the server unless `Mcp.endSessionOnExit` is off, or `--adopt` made an agent session of your own session. The `Mcp` settings set its timeouts and limits.
- `afterglow ctl colors` prints a pane's colors. `set-colors` sets them as its program would, from `NAME=COLOR` words or a theme name or file, on one pane or all with `--all`, after `--reset`.
- `afterglow ctl set-font-size`, `set-opacity`, `set-user-var`, `signal-program` and `window` do what `kitten @ set-font-size`, `set-background-opacity`, `set-user-vars`, `signal-child` and `resize-os-window` do. `signal-program` signals the foreground process group of the pane. That is the running program, or the shell at its prompt. `window` makes a window full screen, maximized or normal, and sizes it in cells.
- `afterglow ctl get-text --ansi` writes pane text with its colors, attributes and links, as kitty-scrollback.nvim reads it. `--cursor` adds the cursor and `--wrap-markers` marks wrapped rows. `--first-output-on-screen` and `--last-visited-output` read the outputs of the command output actions. A text over its limit of 1 MiB keeps the end of the last output, and `get-text` says on stderr that it is cut.
- `afterglow ctl split`, `new-tab` and `new-window` take `--env`, `--var` and `--hold`. `split` and `new-tab` also take `--no-focus`, and `--stdin-text SCOPE`, which gives a program the pane's text on its standard input, as `kitten @ launch --stdin-source` does. `overlay --stdin-text` does too.
- `afterglow ctl screenshot` saves a PNG of a pane, its tab or its window as the window shows it, with the shader packs. With `--window` it includes the tab bar. It needs `Control.allowReadText`.
- Script commands. A script or package declares each command with a summary. A usage text, a palette title and standard input are optional. A script can also change its commands at run time with `afterglow.set_commands`, which takes the same map of names as the package manifest.
- Commands that a script marks `remote` also run from a remote host, with `aglow run` in an `afterglow ssh` session. They need `Terminal.sshRemoteCommands`, which is off by default, and `Control.enabled` and `Scripts.enabled`. `Terminal.sshIntegration` is not necessary, because you can also run `afterglow ssh` yourself.
- `afterglow ssh` forwards a socket of Afterglow for each session over the shared ssh connection. `aglow run` on the host finds it with OSC 24569. Before the session, `aglow remote-prune` removes the sockets of sessions that ended. An `afterglow ssh` in an overlay has a socket of its own, and the pane keeps the socket of its session.
- A `ControlPath none` or `ControlMaster no` in your SSH configuration stays as you set it. That session then gets no socket for remote commands.
- A pane has at most 16 sessions with remote commands, and Afterglow at most 256. The socket of a session takes at most 64 connections at the same time.
- The handler of a remote run gets `ev.source` `"remote"` and the host in `ev.remote.host`. It reads pane text only when `Control.allowRemoteReadText` is on too, which is off by default.
- `aglow run ID COMMAND` runs a command from a shell with its arguments, output and exit status. A command that asks for standard input gets it when standard input is not a terminal. The palette, the context menu, keys and `afterglow ctl action` run commands too. The palette shows a command as `ID: TITLE`. A held key runs a command once.
- `Scripts.commandTimeout` limits the time of a run. `Scripts.commandRunLimit` limits the runs of one script at a time, from 1 to 16. `Scripts.commandStdinLimit` and `Scripts.commandOutputLimit` limit their standard input and output, in KiB. Standard input over its limit is never cut. `aglow run` refuses the run with exit status 2, as it does for standard input that is not UTF-8 text.
- An interrupt or `SIGTERM` cancels the run, and `aglow run` then ends by that signal. So does a closed standard output, with `SIGPIPE`. A canceled run also stops the tasks and the sleeps that it waits for.
- A script command that runs for a client, such as `aglow run`, reads pane text only when `Control.allowReadText` and `Scripts.allowReadText` are on.
- A keybind action from `afterglow ctl action` or `afterglow.action` is refused while a prompt or a script dialog is open.
- Overlays: `afterglow.overlay` runs a program, such as fzf, over a pane and returns its exit status and its output to the script. The program takes the input focus while it runs. It can read a list on standard input, draw on the terminal and answer on standard output or in the file that `AFTERGLOW_RESULT` names. The overlay fades in and out through the `Shaders.overlayTransition` pack, Overlay Fade by default. The pack blends the pane and the program, and both have the pane's layer and pane shaders. `Animation.overlayCurve` and `Animation.overlayDuration` time the fade.
- An overlay closes before its program exits when its pane closes or its script stops. It also closes when its pane of a mux session moves to a new session. It also closes when the command run or the `afterglow ctl` client that waits for it ends. An overlay that a control client opened also closes when `Control.enabled` goes off. The `cancel_overlay` action, Ctrl+Shift+Alt+Escape by default, closes the overlay of the focused pane. In each case, the program gets `SIGHUP`.
- Popup overlays: the `width` and `height` options make an overlay a popup over a part of its pane. Each is a number of cells or a percentage of the pane. `position` puts the popup at one of nine positions, and `border` adds or removes its border. The pane stays visible and keeps running around the popup, darker by `Colors.popupDim`. A border in the popup border color shows the title in its top edge, in the text color. `Window.popupCorners` makes its corners rounded or square. The mouse acts in the popup only. A click on the pane around it gives that pane the focus and reaches no program.
- `afterglow ctl overlay -- CMD` runs a program over a pane from a shell script. It uses the `overlay` call of the control socket. It sends standard input of up to 4 MiB to the program, in parts before the call. It prints the program's output and exits with its status. Without `Control.allowReadText`, a control client gets the exit status but not the output, and `afterglow ctl overlay` prints nothing on standard output. `-w`, `-h` or `--border` makes it a popup, at `--position`. `--no-border` removes the border of a popup.
- The fzf picker example package picks a saved project or a file with fzf in a popup overlay. Without fzf, it uses the select dialog.
- Shell completion of `afterglow` and `aglow` for Bash, Zsh and Fish, with the commands of the running scripts. The completion reads the commands from `aglow run`, which starts faster than `afterglow`. The Fish completion needs Fish 4 or later.
- A launch that arrives while the first Afterglow process starts waits until that process is ready, then opens its window there.
- A different build on the same display starts on its own and does not hand its window over.

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
- Accessibility clients can run the pane actions of the command palette, open the context menu of a pane and give a pane the focus. They can also press the items of a script's docked area. Pane actions do nothing while a prompt or a keyboard mode has the keys.
- Input-method support with surrounding text for the cursor row.
- Tabs, menus and prompts in the desktop Qt style.
- Settings in `~/.config/phosphor-works/afterglow/config.json` that reload when the file changes, and `-o` overrides for one launch.
- With no settings file, every setting has its default. The first change of a setting creates the file, which holds only the values that differ from the defaults. Only its owner can read the file.
- Problems in the settings file show as a notice. Examples are an unknown key, a value of the wrong type and a value that is not a color.
- A settings file that does not parse is never replaced. The current settings stay until it is correct. At start, Afterglow then uses the defaults, with `Scripts.enabled` and `Control.enabled` off until the file holds a JSON object or is deleted. A file that becomes empty before that does not turn them on. An `-o` override of either key with a boolean value still sets it.
- `--config FILE` reads another settings file. The `themes`, `shaders`, `shader-presets` and `scripts` directories beside it take the place of your own.
- The desktop's fixed-width font and size by default, with the `calt` and `liga` features. `Font.sizeStep` sets the font size step.
- A clean exit on `SIGTERM`, `SIGINT` and `SIGHUP` that closes the windows and the control socket. A signal that Afterglow inherited as ignored stays ignored.

### Documentation

- [Documentation index](docs/README.md).
- [Supported protocols and limits](docs/COMPATIBILITY.md).
- [Desktop integration](docs/DESKTOP.md) for accessibility, input methods, file and text drops, Kitty file transfer, Kitty drag and drop, and the `aglow` client.
- [Bidirectional text](docs/BIDI.md) for the application-grid policy.
- [External control](docs/CONTROL.md) for `afterglow ctl`, the control socket and script commands from a shell.
- [The mux daemon](docs/MUX.md) for `afterglow mux` and the sessions that it keeps.
- [The MCP server](docs/MCP.md) for `afterglow mcp` and the tools that it gives an agent.
- [Testing](docs/TESTING.md) for regression and native desktop test procedures.
