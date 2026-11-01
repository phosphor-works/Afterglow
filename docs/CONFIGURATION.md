<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Configuration reference

`tools/generate-reference.py` generates this document from the runtime Phosphor schema. Group and key names are case-sensitive. See [appearance](APPEARANCE.md) for configuration loading and examples.

Numeric values are clamped to the listed ranges. An integer key drops the fraction of a value, and uses its default for a value outside the 32-bit range. Invalid enum choices use the default. Where an empty string has a special meaning, the description says so. Omitted values use defaults.

## Animation

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `bellCurve` | `"spring:14,1"` |  | Curve of the visual bell fade, in the splitCurve format. |
| `bellDuration` | `150` | 16–5000 | Duration of the visual bell fade in milliseconds for curves other than springs. |
| `cursor` | `true` |  | Animate cursor movement between cells. |
| `cursorCurve` | `"spring:48,1"` |  | Curve of cursor motion between cells, in the splitCurve format. |
| `cursorDuration` | `150` | 16–5000 | Duration of cursor motion in milliseconds for curves other than springs. |
| `overviewCurve` | `"spring:12,0.82"` |  | Curve of the pane overview motion, in the splitCurve format. The default spring overshoots slightly. |
| `overviewDuration` | `450` | 16–5000 | Duration of pane overview motion in milliseconds for curves other than springs. |
| `paneEffectDuration` | `420` | 16–5000 | Milliseconds a pane takes to open or close in Shaders.paneEffects. |
| `scroll` | `true` |  | Animate scrolling between rows. |
| `scrollCurve` | `"spring:26,1"` |  | Curve of scroll motion, in the splitCurve format. |
| `scrollDuration` | `150` | 16–5000 | Duration of scroll motion in milliseconds for curves other than springs. |
| `splitCurve` | `"spring:22,1"` |  | Curve of split animations, as a Phosphor curve spec. Use a spring such as spring:22,1 (angular frequency, damping ratio), a cubic Bezier such as 0.2,0,0,1, or a named easing such as elastic-out. An invalid spec uses the default. |
| `splitDuration` | `250` | 16–5000 | Duration of split animations in milliseconds for curves other than springs, which settle by themselves. |
| `splits` | `true` |  | Animate split opening, closing and resizing. |
| `tabCurve` | `"spring:13,0.9"` |  | Curve of tab switches (Shaders.tabTransition), in the splitCurve format. |
| `tabDuration` | `400` | 16–5000 | Duration of tab switches in milliseconds for curves other than springs. |

## Audio

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `enabled` | `true` |  | Analyze system playback when a visible shader requests audio. Requires cava. |
| `extraSmoothing` | `0.2` | 0–0.95 | Part of the previous spectrum kept on each update, after noise reduction. A value of 0 turns it off. |
| `framerate` | `60` | 30–144 | Spectrum updates per second. |
| `higherCutoff` | `10000` | 1000–20000 | Highest analyzed frequency in hertz. |
| `inputMethod` | `"auto"` | `"auto"`, `"pipewire"`, `"pulse"` | Sound system that cava captures from. The auto value detects it. |
| `inputSource` | `"auto"` |  | Playback source for capture. The auto value follows the default output. |
| `lowerCutoff` | `50` | 20–500 | Lowest analyzed frequency in hertz. |
| `smoothing` | `65` | 0–100 | Noise reduction of the spectrum analyzer. A value of 0 is fast and jumpy. 100 is slow and smooth. |

## Colors

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `background` | `"#1c1c1c"` |  | Default background color. |
| `boldIsBright` | `false` |  | Use bright colors 8–15 for bold text with colors 0–7. |
| `cursor` | `""` |  | Cursor color. An empty value uses the foreground. |
| `cursorText` | `""` |  | Text color under a block cursor. An empty value uses the background. |
| `divider` | `"#3a3a42"` |  | Color of the dividers between panes. A theme without a divider color gets one between its background and text colors. |
| `foreground` | `"#d8d8d8"` |  | Default text color. |
| `minimumContrast` | `1` | 1–21 | Minimum text contrast ratio. Adjust text below this ratio. A value of 1 preserves colors. 4.5 is the WCAG AA normal-text threshold. |
| `palette` | `["#1c1c1c", "#cc5555", "#55cc55", "#cdcd55", "#5455cb", "#cc55cc", "#7acaca", "#cccccc", "#555555", "#ff5555", "#55ff55", "#ffff55", "#5555ff", "#ff55ff", "#55ffff", "#ffffff"]` |  | The 16 ANSI colors, from black to bright white. |
| `searchCurrentMatch` | `"#ff8c42"` |  | Background color of the current search match. |
| `searchMatch` | `"#e5c07b"` |  | Background color of search matches. |
| `searchMatchText` | `"#101010"` |  | Text color of search matches. |
| `selectionBackground` | `""` |  | Selection background color. An empty value reverses the cell colors. |
| `selectionForeground` | `""` |  | Selected text color. An empty value reverses the cell colors. |
| `theme` | `""` |  | Theme name or file path. Afterglow searches ~/.config/afterglow/themes, then afterglow/themes in each XDG data directory, then the bundled themes, such as nord and afterglow-light. Use light:NAME,dark:NAME to follow the desktop. A theme replaces each color that it sets. Afterglow reads its own JSON themes, Ghostty themes and Kitty themes. A relative path starts at the directory of the config file. |
| `unfocusedDim` | `0.19` | 0–1 | Opacity of a black layer over unfocused panes. A value of 0 turns the darkening off. |
| `visualBell` | `"#ffffff"` |  | Color of the visual bell flash. A color that a program sets with OSC 21 replaces it. |
| `visualBellIntensity` | `0.18` | 0–1 | Opacity of the visual bell flash when it starts. A value of 0 hides the flash. |

## Control

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `allowReadText` | `false` |  | Allow D-Bus clients to read pane text, including scrollback. |
| `enabled` | `true` |  | Allow other programs to control Afterglow through D-Bus and afterglow ctl. |

## Font

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `antialiasing` | `"grayscale"` | `"grayscale"`, `"none"`, `"rgb"`, `"bgr"` | How glyph edges are smoothed. Subpixel modes use grayscale when transparency or shaders are active. Pane effects are shaders, and they run while a pane is remote or in another context. |
| `boldFamily` | `""` |  | Font family of bold text. An empty value uses family. |
| `boldItalicFamily` | `""` |  | Font family of bold italic text. An empty value uses the italic family. |
| `boldWeight` | `700` | 100–900 | Weight of bold text. It selects a static font and sets the weight axis of a variable font. |
| `cellHeight` | `1` | 0.5–3 | Row-height multiplier relative to font metrics. A value of 1.2 adds 20 percent to the row height. |
| `cellWidth` | `1` | 0.5–3 | Column-width multiplier relative to font metrics. |
| `fallbackFamilies` | `[]` |  | Fallback families in priority order. These supply missing characters before system fallback. |
| `family` | `""` |  | Font family of regular text. An empty value uses the desktop's fixed-width font. |
| `features` | `["calt", "liga"]` |  | OpenType features in HarfBuzz syntax, such as calt, liga, ss01, cv01=2 or -liga. |
| `gamma` | `1` | 0.5–3 | Glyph coverage gamma. A value of 1 preserves coverage. Higher values increase edge coverage. |
| `hinting` | `"light"` | `"none"`, `"light"`, `"full"` | How glyph outlines align to the pixel grid. The none mode preserves outlines. The light mode aligns vertical positions only. The full mode aligns both axes. |
| `italicFamily` | `""` |  | Font family of italic text. An empty value uses family. |
| `italicSlant` | `-11` | -90–90 | Slant of italic text in degrees, for variable fonts with a slant axis. |
| `size` | `0` | 0–96 | Size in points, from 4 to 96. A value of 0 uses the size of the desktop's fixed-width font. |
| `sizeStep` | `1` | 0.1–12 | Points that the font_bigger and font_smaller actions add or remove. |
| `symbolMap` | `{}` |  | Preferred families for codepoint ranges, such as { "U+E000-U+F8FF,U+F0000-U+FFFFD": "Symbols Nerd Font Mono" }. Afterglow draws box drawing, block elements, Braille and the Powerline separators U+E0B0 to U+E0BF itself, so this setting does not change them. |
| `weight` | `400` | 100–900 | Weight of regular text. It selects a static font and sets the weight axis of a variable font. |
| `width` | `100` | 50–200 | Width axis of variable fonts. A value of 100 is the normal width. Each font limits it to its own range. |

## Graphics

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `quota` | `320` | 16–4096 | Mebibytes of Kitty graphics images that each screen of a pane keeps. A pane has a main and an alternate screen. The oldest images go first when a screen is full. |

## Hints

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `alphabet` | `"asdfjklghqweruiopzxcvbnmty"` |  | Letters that label hints and URL hints, the easiest to reach first. It needs two or more letters, each one time. Other values use the default. |
| `patterns` | `[{"action": "copy", "pattern": "(?:~?/\|\\.{1,2}/)?[\\w.@+-]+(?:/[\\w.@+-]+)*(?:\\.[\\w+-]+\|:\\d+)(?::\\d+){0,2}"}]` |  | Up to 16 hints. Each is an object with a regular expression in pattern and an action: copy, open or command. A command action also needs command, a list of 1 to 32 arguments. |

## Keybinds

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `bindings` | [default bindings](#default-bindings) |  | Map each action to a key combination or list. An empty string removes the binding. |
| `leader` | `""` |  | Key that Leader stands for in sequences. It has no default. |
| `sequenceTimeout` | `1000` | 100–10000 | Milliseconds before an incomplete sequence is cancelled. |
| `tableTimeout` | `5000` | 100–60000 | Idle milliseconds before leaving a key table. |
| `tables` | `{}` |  | Named action-to-key maps, activated with table:NAME. |

## Links

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `detectUrls` | `true` |  | Detect plain-text URLs in addition to application-provided OSC 8 links. |
| `modifier` | `"ctrl"` | `"ctrl"`, `"shift"`, `"alt"`, `"none"` | Modifier for link highlighting and activation. With none, a plain click opens the link. |
| `openWith` | `""` |  | Command for opening links. The URL is appended. An empty value uses the desktop browser. |
| `schemes` | `["http", "https", "mailto", "ftp", "file"]` |  | URL schemes that open without a prompt. Links with other schemes ask first. |

## Mouse

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `middleClick` | `"primary"` | `"primary"`, `"clipboard"`, `"none"` | What a middle click pastes. The none value turns middle-click paste off. |
| `rightClick` | `"menu"` | `"menu"`, `"paste"`, `"copy-or-paste"`, `"extend"` | What a right click does. The copy-or-paste action copies an existing selection or pastes otherwise. The extend action extends the selection. |

## Notifications

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `rateLimit` | `20` | 0–1000 | Desktop notifications that one pane can show in 10 seconds. A value of 0 removes the limit. Notices from Afterglow itself do not count. |

## QuickTerminal

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `animate` | `true` |  | Animate quick-terminal opening and closing. |
| `curve` | `"spring:22,1"` |  | Curve of the quick terminal slide, in the Animation.splitCurve format. |
| `duration` | `150` | 16–5000 | Duration of the quick terminal slide in milliseconds for curves other than springs. |
| `height` | `0.4` | 0.1–1 | Height as a fraction of the screen. |
| `position` | `"top"` | `"top"`, `"bottom"` | Screen edge the quick terminal slides from. |
| `screen` | `""` |  | Output that shows the quick terminal, such as DP-1. An empty or unknown name uses the primary screen. |
| `shortcut` | `` "Meta+`" `` |  | Global shortcut to show or hide the quick terminal. An empty value disables the shortcut. |

## Scripts

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `allowReadText` | `true` |  | Allow local Luau scripts to query pane text, selections and semantic regions. |
| `enabled` | `true` |  | Run Luau scripts from ~/.config/afterglow/scripts and from afterglow/scripts in each XDG data directory. |

## Shaders

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `background` | `[]` |  | Shaders run over each pane's background layer alone: cell backgrounds and images under the text. |
| `cursor` | `[]` |  | Shaders run over each pane's cursor layer alone. |
| `overview` | `"pane-deck"` |  | Shader that draws the pane overview (pane_overview). An empty value turns the overview off. |
| `pane` | `[]` |  | Shaders run over each pane. Name a pack by its directory name, which is normally its id, such as cursor-trail or bloom. |
| `paneEffects` | `"pane-effects"` |  | Shader that animates panes as they split open and close. An empty value turns it off. |
| `params` | `{}` |  | Parameter values for each shader reference, such as { "bloom": { "strength": 0.5 } }. |
| `tabTransition` | `"tab-switch"` |  | Shader that animates switching tabs. An empty value switches at once. |
| `text` | `[]` |  | Shaders run over each pane's text layer alone: glyphs, decorations and images over the text. |
| `window` | `[]` |  | Shaders run over the whole window, across splits, after the pane shaders. |

## Terminal

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `allowScreenReadback` | `false` |  | Answer DECRQCRA checksum requests. These let any program in the pane read the screen back. |
| `ambiguousWide` | `false` |  | Use two cells for East Asian Ambiguous characters, including Greek, Cyrillic and some symbols. |
| `bell` | `"attention"` | `"none"`, `"attention"`, `"visual"`, `"both"` | Response to the bell. The attention value marks the window urgent. The visual value flashes the pane. The both value does both. |
| `bellSound` | `""` |  | Bell sound. An empty value disables sound. Use default for the desktop bell, or specify a sound file. |
| `bidi` | `"off"` | `"off"`, `"auto"`, `"ltr"`, `"rtl"` | Bidirectional paragraph layout mode. Application grids retain logical order. |
| `clipboardRead` | `"ask"` | `"ask"`, `"allow"`, `"deny"` | Programs reading the clipboard (OSC 52, OSC 5522). The ask value shows a prompt for each read. |
| `clipboardWrite` | `"allow"` | `"allow"`, `"ask"`, `"deny"` | Programs writing the clipboard (OSC 52, OSC 5522). The ask value shows a prompt for each write. |
| `confirmClose` | `true` |  | Ask before closing a pane, tab or window where a program is running. |
| `confirmUnsafePaste` | `true` |  | Ask before pasting text that holds a line break, unless the program uses bracketed paste. A line break could run a command. |
| `copyOnSelect` | `"primary"` | `"primary"`, `"clipboard"`, `"off"` | Where new selections are copied: the primary selection, the clipboard too, or nowhere with off. |
| `cursorBlink` | `true` |  | Make the cursor blink. |
| `cursorBlinkInterval` | `530` | 100–5000 | Duration in milliseconds for each visible and hidden cursor phase. |
| `cursorShape` | `"block"` | `"block"`, `"underline"`, `"bar"` | Shape of the cursor until a program sets another one with DECSCUSR. |
| `env` | `{}` |  | Variables added to the shell's environment, such as { "EDITOR": "nvim" }. Each value must be a string. |
| `fileTransferPassword` | `""` |  | Shared secret for Kitty file-transfer approval. An empty value always asks for approval. |
| `graphicsFileTransfer` | `"allow"` | `"allow"`, `"deny"` | Images that programs send with the Kitty graphics protocol as a file, a temporary file or shared memory. Images sent inline always work. |
| `hideMouseWhileTyping` | `false` |  | Hide the pointer while typing. Pointer movement shows it again. |
| `lastOutputLimit` | `1024` | 1–65536 | Kibibytes of the last command output that the pager gets. For longer output, the pager gets the start and a note. |
| `notifications` | `true` |  | Show desktop notifications that programs send with OSC 9, OSC 99 and OSC 777, and program status notifications (OSC 7501). |
| `pager` | `"less -R"` |  | Command for viewing the last command output. Shell expansion is disabled. |
| `scrollLines` | `3` | 1–50 | Number of rows scrolled for each mouse-wheel notch. |
| `scrollOnInput` | `true` |  | Scroll to the bottom on typing or paste. |
| `scrollOnOutput` | `false` |  | Scroll to the bottom when new output arrives. |
| `scrollback` | `10000` | 0–1000000 | Lines of history that each pane keeps. |
| `shell` | `""` |  | Program to run. An empty value starts your login shell. |
| `shellIntegration` | `true` |  | Integrate Bash, Zsh and Fish prompts in new panes automatically. |
| `sshIntegration` | `true` |  | Run ssh through afterglow ssh in integrated shells. The pane label then shows the host, and the session offers COLORTERM and TERM_PROGRAM to the server. |
| `sshTerminfo` | `"install"` | `"install"`, `"fallback"` | What to do when TERM is afterglow: install its terminfo entry on each ssh host, or use xterm-256color there instead. |
| `term` | `"xterm-256color"` |  | Value of TERM for the programs in the panes. |
| `textBlinkInterval` | `500` | 100–5000 | Duration in milliseconds for each visible and hidden phase of blinking text (SGR 5). |
| `wordSeparators` | `` "`\"'│\|:;,()[]{}<>" `` |  | Characters that end a word for double-click selection, besides spaces. |

## Window

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `backgroundOpacity` | `1` | 0.05–1 | Opacity of the default background. Text stays opaque. |
| `balancePadding` | `false` |  | Distribute unused cell space between opposite edges. Otherwise, space remains at the right and bottom. |
| `blur` | `false` |  | Blur content behind translucent backgrounds with KWin. |
| `decorations` | `true` |  | Show the compositor's title bar and borders on new windows. |
| `initialColumns` | `100` | 10–1000 | Columns of a new window. |
| `initialRows` | `30` | 4–500 | Rows of a new window. |
| `padding` | `6` | 0–200 | Space around the grid in logical pixels. |
| `paneTitles` | `false` |  | Show pane titles in tabs with two or more panes. |
| `scrollbar` | `"auto"` | `"auto"`, `"always"`, `"never"` | When the history scrollbar shows. The auto value shows the scrollbar while the view scrolls or the pointer is over it, then fades it out. |
| `scrollbarHideDelay` | `1200` | 0–60000 | Milliseconds an auto scrollbar stays visible after the last scroll or after the pointer leaves. |
| `startMode` | `"normal"` | `"normal"`, `"maximized"`, `"fullscreen"` | How new windows open. |
| `tabBar` | `"auto"` | `"auto"`, `"always"`, `"never"` | When the tab bar shows. The auto value shows it while there are two or more tabs. |
| `workingDirectory` | `"inherit"` |  | Starting directory for new tabs, panes and windows. Use inherit, home or a path. Inherit uses the focused pane's directory or the launcher's directory. The afterglow -d option overrides this setting. |

## Default bindings

| Action | Keys |
| --- | --- |
| `clear_screen` | `"Ctrl+Shift+K"` |
| `close_pane` | `"Ctrl+Shift+W"` |
| `close_tab` | `"Ctrl+Shift+Q"` |
| `close_window` | `[]` |
| `command_palette` | `"Ctrl+Shift+P"` |
| `copy` | `"Ctrl+Shift+C"` |
| `copy_mode` | `"Ctrl+Shift+Space"` |
| `detach_tab` | `[]` |
| `end_script_key_mode` | `"Ctrl+Shift+Escape"` |
| `equalize_splits` | `"Ctrl+Shift+Alt+="` |
| `focus_down` | `"Ctrl+Shift+Down"` |
| `focus_left` | `"Ctrl+Shift+Left"` |
| `focus_right` | `"Ctrl+Shift+Right"` |
| `focus_up` | `"Ctrl+Shift+Up"` |
| `font_bigger` | `["Ctrl+=", "Ctrl++"]` |
| `font_reset` | `"Ctrl+0"` |
| `font_smaller` | `"Ctrl+-"` |
| `goto_tab_1` | `"Alt+1"` |
| `goto_tab_2` | `"Alt+2"` |
| `goto_tab_3` | `"Alt+3"` |
| `goto_tab_4` | `"Alt+4"` |
| `goto_tab_5` | `"Alt+5"` |
| `goto_tab_6` | `"Alt+6"` |
| `goto_tab_7` | `"Alt+7"` |
| `goto_tab_8` | `"Alt+8"` |
| `goto_tab_9` | `"Alt+9"` |
| `jump_prompt_down` | `"Ctrl+Shift+X"` |
| `jump_prompt_up` | `"Ctrl+Shift+Z"` |
| `move_pane_to_tab` | `[]` |
| `move_pane_to_window` | `[]` |
| `move_tab_left` | `"Ctrl+Shift+Alt+PgUp"` |
| `move_tab_right` | `"Ctrl+Shift+Alt+PgDown"` |
| `new_tab` | `"Ctrl+Shift+T"` |
| `new_window` | `"Ctrl+Shift+N"` |
| `next_tab` | `["Ctrl+Tab", "Ctrl+PgDown"]` |
| `notify_on_finish` | `[]` |
| `open_config` | `"Ctrl+Shift+,"` |
| `pane_overview` | `"Ctrl+Shift+D"` |
| `paste` | `"Ctrl+Shift+V"` |
| `previous_tab` | `["Ctrl+Shift+Tab", "Ctrl+PgUp"]` |
| `reload_config` | `"Ctrl+Shift+R"` |
| `rename_pane` | `[]` |
| `rename_tab` | `[]` |
| `reset` | `[]` |
| `resize_down` | `"Ctrl+Shift+Alt+Down"` |
| `resize_left` | `"Ctrl+Shift+Alt+Left"` |
| `resize_right` | `"Ctrl+Shift+Alt+Right"` |
| `resize_up` | `"Ctrl+Shift+Alt+Up"` |
| `rotate_panes_clockwise` | `"Ctrl+Shift+]"` |
| `rotate_panes_counterclockwise` | `"Ctrl+Shift+["` |
| `save_project` | `[]` |
| `scroll_bottom` | `"Shift+End"` |
| `scroll_page_down` | `"Shift+PgDown"` |
| `scroll_page_up` | `"Shift+PgUp"` |
| `scroll_top` | `"Shift+Home"` |
| `search` | `"Ctrl+Shift+F"` |
| `select_all` | `"Ctrl+Shift+A"` |
| `show_hints` | `"Ctrl+Shift+H"` |
| `show_last_output` | `[]` |
| `show_urls_copy` | `"Ctrl+Shift+Y"` |
| `show_urls_open` | `"Ctrl+Shift+G"` |
| `split_down` | `"Ctrl+Shift+O"` |
| `split_left` | `[]` |
| `split_right` | `"Ctrl+Shift+E"` |
| `split_up` | `[]` |
| `swap_down` | `[]` |
| `swap_left` | `[]` |
| `swap_right` | `[]` |
| `swap_up` | `[]` |
| `switch_project` | `[]` |
| `toggle_fullscreen` | `"F11"` |
| `toggle_pane_zoom` | `"Ctrl+Shift+Return"` |
| `unicode_input` | `"Ctrl+Shift+U"` |
| `use_package_shaders` | `[]` |
