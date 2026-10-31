<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Configuration reference

`tools/generate-reference.py` generates this document from the runtime Phosphor schema. Group and key names are case-sensitive. See [appearance](APPEARANCE.md) for configuration loading and examples.

Numeric values are clamped to the listed ranges. Invalid enum choices use the default. Each description defines the meaning of an empty string. Omitted values use defaults.

## Animation

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `cursor` | `true` |  | Animate cursor movement between cells. |
| `scroll` | `true` |  | Animate scrolling between rows. |
| `splits` | `true` |  | Animate split opening, closing and resizing. |

## Audio

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `enabled` | `true` |  | Analyze system playback when a visible shader requests audio. Requires cava. |
| `inputMethod` | `"auto"` | `"auto"`, `"pipewire"`, `"pulse"` | Audio capture backend |
| `inputSource` | `"auto"` |  | Playback source for capture. The auto value follows the default output. |

## Colors

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `background` | `"#1c1c1c"` |  | Default background color. |
| `boldIsBright` | `false` |  | Use bright colors 8–15 for bold text with colors 0–7. |
| `cursor` | `""` |  | Cursor color. An empty value uses the foreground. |
| `cursorText` | `""` |  | Text color under a block cursor. An empty value uses the background. |
| `divider` | `"#3a3a42"` |  | Split divider color. |
| `foreground` | `"#d8d8d8"` |  | Default text color. |
| `minimumContrast` | `1` | 1–21 | Minimum text contrast ratio. Adjust text below this ratio. A value of 1 preserves colors. 4.5 is the WCAG AA normal-text threshold. |
| `palette` | `["#1c1c1c", "#cc5555", "#55cc55", "#cdcd55", "#5455cb", "#cc55cc", "#7acaca", "#cccccc", "#555555", "#ff5555", "#55ff55", "#ffff55", "#5555ff", "#ff55ff", "#55ffff", "#ffffff"]` |  | The 16 ANSI colors, from black to bright white. |
| `selectionBackground` | `""` |  | Selection background color. An empty value reverses the cell colors. |
| `selectionForeground` | `""` |  | Selected text color. An empty value reverses the cell colors. |
| `theme` | `""` |  | Theme name from ~/.config/afterglow/themes or bundled themes: afterglow-dark and afterglow-light. Use light:NAME,dark:NAME to follow the desktop. Theme colors replace explicit colors. Supports Afterglow JSON, Ghostty and Kitty themes. |
| `unfocusedDim` | `0.19` | 0–1 | Darkening amount for unfocused splits. |

## Control

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `allowReadText` | `false` |  | Allow D-Bus clients to read pane text, including scrollback. |
| `enabled` | `true` |  | Allow other programs to control Afterglow through D-Bus and afterglow ctl. |

## Font

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `antialiasing` | `"grayscale"` | `"grayscale"`, `"none"`, `"rgb"`, `"bgr"` | Font antialiasing mode. Subpixel modes use grayscale when transparency or shaders are active. |
| `boldFamily` | `""` |  | Bold font family. An empty value uses family. |
| `boldItalicFamily` | `""` |  | Bold italic font family. An empty value uses the italic family. |
| `boldWeight` | `700` | 100–900 | Bold font weight. Used for static font matching and variable font axes. |
| `cellHeight` | `1` | 0.5–3 | Row-height multiplier relative to font metrics. A value of 1.2 adds 20 percent to the row height. |
| `cellWidth` | `1` | 0.5–3 | Column-width multiplier relative to font metrics. |
| `fallbackFamilies` | `[]` |  | Fallback families in priority order. These supply missing characters before system fallback. |
| `family` | `"Monaspace Neon Var"` |  | Regular text font family |
| `features` | `["calt", "liga", "ss01", "ss02", "ss03", "ss04", "ss05", "ss06", "ss07", "ss08", "ss09", "ss10"]` |  | OpenType features in HarfBuzz syntax: calt, liga, ss01, cv01=2, -liga |
| `gamma` | `1` | 0.5–3 | Glyph coverage gamma. A value of 1 preserves coverage. Higher values increase edge coverage. |
| `hinting` | `"light"` | `"none"`, `"light"`, `"full"` | Glyph hinting mode. The none mode preserves outlines. The light mode aligns vertical positions only. The full mode aligns both axes. |
| `italicFamily` | `""` |  | Italic font family. An empty value uses family. |
| `italicSlant` | `-11` | -90–90 | Slant of italic text (variable fonts) |
| `size` | `11` | 4–96 | Size in points |
| `symbolMap` | `{}` |  | Preferred families for codepoint ranges. Example: { "U+E000-U+F8FF,U+F0000-U+FFFFD": "Symbols Nerd Font Mono" }. Afterglow draws box and powerline shapes itself. |
| `weight` | `400` | 100–900 | Regular font weight. Used for static font matching and variable font axes. |
| `width` | `100` | 50–200 | Width axis (Monaspace: 100-125) |

## Hints

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `patterns` | `[{"action": "copy", "pattern": "(?:~?/\|\\.{1,2}/)?[\\w.@+-]+(?:/[\\w.@+-]+)*(?:\\.[\\w+-]+\|:\\d+)(?::\\d+){0,2}"}]` |  | Up to 16 regular-expression hints with copy, open or command actions. A command uses an argv list. |

## Keybinds

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `bindings` | [default bindings](#default-bindings) |  | Map each action to a key combination or list. An empty string removes the binding. |
| `leader` | `""` |  | Prefix key used by Leader sequences |
| `sequenceTimeout` | `1000` | 100–10000 | Milliseconds before an incomplete sequence is cancelled |
| `tableTimeout` | `5000` | 100–60000 | Idle milliseconds before leaving a key table |
| `tables` | `{}` |  | Named action-to-key maps, activated with table:name |

## Links

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `detectUrls` | `true` |  | Detect plain-text URLs in addition to application-provided OSC 8 links. |
| `modifier` | `"ctrl"` | `"ctrl"`, `"shift"`, `"alt"`, `"none"` | Modifier for link highlighting and activation. With none, a plain click opens the link. |
| `openWith` | `""` |  | Command for opening links. The URL is appended. An empty value uses the desktop browser. |

## Mouse

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `middleClick` | `"primary"` | `"primary"`, `"clipboard"`, `"none"` | Middle-click action. Paste the primary selection, paste the clipboard, or do nothing. |
| `rightClick` | `"menu"` | `"menu"`, `"paste"`, `"copy-or-paste"`, `"extend"` | Right-click action. The copy-or-paste action copies an existing selection or pastes otherwise. The extend action extends the selection. |

## QuickTerminal

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `animate` | `true` |  | Animate quick-terminal opening and closing. |
| `height` | `0.4` | 0.1–1 | Height as a fraction of the screen |
| `shortcut` | `"Meta+&#96;"` |  | Global shortcut to show or hide the quick terminal. An empty value disables the shortcut. |

## Scripts

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `allowReadText` | `true` |  | Allow local Luau scripts to query pane text, selections and semantic regions |
| `enabled` | `true` |  | Run Luau scripts from ~/.config/afterglow/scripts |

## Shaders

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `background` | `[]` |  | Shaders run over each pane's background layer alone: cell backgrounds and images under the text |
| `cursor` | `[]` |  | Shaders run over each pane's cursor layer alone |
| `pane` | `[]` |  | Shaders run over each pane: pack ids (cursor-trail, bloom) or paths to Ghostty shader files |
| `params` | `{}` |  | Per shader parameter values, e.g. { "bloom": { "strength": 0.5 } } |
| `text` | `[]` |  | Shaders run over each pane's text layer alone: glyphs, decorations and images over the text |
| `window` | `[]` |  | Shaders run over the whole window, across splits, after the pane shaders |

## Terminal

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `ambiguousWide` | `false` |  | Use two cells for East Asian Ambiguous characters, including Greek, Cyrillic and some symbols. |
| `bell` | `"attention"` | `"none"`, `"attention"`, `"visual"`, `"both"` | none, attention (mark the window urgent), visual (flash the pane) or both |
| `bellSound` | `""` |  | Bell sound. An empty value disables sound. Use default for the desktop bell, or specify a sound file. |
| `bidi` | `"off"` | `"off"`, `"auto"`, `"ltr"`, `"rtl"` | Bidirectional paragraph layout mode. Application grids retain logical order. |
| `clipboardRead` | `"ask"` | `"ask"`, `"allow"`, `"deny"` | Programs reading the clipboard (OSC 52): ask, allow or deny |
| `confirmClose` | `true` |  | Ask before closing a split, tab or window where a program is running |
| `confirmUnsafePaste` | `true` |  | Ask before pasting several lines into a program that would run them |
| `copyOnSelect` | `"primary"` | `"primary"`, `"clipboard"`, `"off"` | Copy new selections to primary, both primary and clipboard, or neither with off. |
| `cursorBlink` | `true` |  | Blink the cursor |
| `cursorBlinkInterval` | `530` | 100–5000 | Duration in milliseconds for each visible and hidden cursor phase. |
| `cursorShape` | `"block"` | `"block"`, `"underline"`, `"bar"` | block, underline or bar |
| `env` | `{}` |  | Variables added to the shell's environment, e.g. { "EDITOR": "nvim" } |
| `fileTransferPassword` | `""` |  | Shared secret for Kitty file-transfer approval. Empty always asks for approval. |
| `hideMouseWhileTyping` | `false` |  | Hide the pointer while typing. Pointer movement shows it again. |
| `notifications` | `true` |  | Show desktop notifications programs send (OSC 9, OSC 777) |
| `pager` | `"less -R"` |  | Command for viewing the last command output. Shell expansion is disabled. |
| `scrollLines` | `3` | 1–50 | Number of rows scrolled for each mouse-wheel notch. |
| `scrollOnInput` | `true` |  | Scroll to the bottom on typing or paste. |
| `scrollOnOutput` | `false` |  | Scroll to the bottom when new output arrives. |
| `scrollback` | `10000` | 0–1000000 | Lines of history per pane |
| `shell` | `""` |  | Program to run. An empty value starts your login shell. |
| `shellIntegration` | `true` |  | Automatic Bash, Zsh and Fish prompt integration |
| `term` | `"xterm-256color"` |  | TERM for child programs |
| `wordSeparators` | `"&#96;\"'│\|:;,()[]{}<>"` |  | Characters that end a word for double-click selection, besides spaces |

## Window

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `backgroundOpacity` | `1` | 0.05–1 | Opacity of the default background. Text stays opaque |
| `balancePadding` | `false` |  | Distribute unused cell space between opposite edges. Otherwise, space remains at the right and bottom. |
| `blur` | `false` |  | Blur content behind translucent backgrounds with KWin. |
| `decorations` | `true` |  | The compositor's title bar and borders (new windows) |
| `initialColumns` | `100` | 10–1000 | Columns of a new window |
| `initialRows` | `30` | 4–500 | Rows of a new window |
| `padding` | `6` | 0–200 | Space around the grid in logical pixels |
| `paneTitles` | `false` |  | Show pane titles in tabs with multiple splits. |
| `startMode` | `"normal"` | `"normal"`, `"maximized"`, `"fullscreen"` | How new windows open: normal, maximized or fullscreen |
| `tabBar` | `"auto"` | `"auto"`, `"always"`, `"never"` | Tab-bar visibility. The auto value shows the bar when multiple tabs exist. |
| `workingDirectory` | `"inherit"` |  | Starting directory for new tabs, splits and windows. Use inherit, home or a path. Inherit uses the focused pane's directory or launcher's directory. The afterglow -d option overrides this setting. |

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
| `equalize_splits` | `"Ctrl+Shift+="` |
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
