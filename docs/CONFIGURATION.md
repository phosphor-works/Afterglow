<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Configuration reference

Generated from the runtime Phosphor schema by `tools/generate-reference.py`. Group and key names are case-sensitive. See the [user guide](README.md) for loading, examples and live reload.

Numeric values are clamped to the listed ranges. Invalid enum choices fall back to the default. Empty strings retain the meaning in each description. Omitted values use defaults.

## Animation

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `cursor` | `true` |  | The cursor glides between cells |
| `scroll` | `true` |  | Scrolling eases instead of jumping by rows |
| `splits` | `true` |  | Splits open, close and resize smoothly |

## Audio

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `enabled` | `true` |  | Analyze system playback while a visible shader opts into audio (requires cava) |
| `inputMethod` | `"auto"` | `"auto"`, `"pipewire"`, `"pulse"` | Audio capture backend |
| `inputSource` | `"auto"` |  | Playback source for the capture backend; auto follows the default output |

## Colors

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `background` | `"#1c1c1c"` |  | Default background colour |
| `boldIsBright` | `false` |  | Bold text in colours 0-7 uses their bright versions, 8-15 |
| `cursor` | `""` |  | Cursor colour; empty follows the foreground |
| `cursorText` | `""` |  | Text under a block cursor; empty uses the background |
| `divider` | `"#3a3a42"` |  | Split divider colour |
| `foreground` | `"#d8d8d8"` |  | Default text colour |
| `minimumContrast` | `1` | 1–21 | Lowest contrast ratio between text and its background; text below it is lightened or darkened. 1 leaves colours alone, 4.5 is WCAG AA |
| `palette` | `["#1c1c1c", "#cc5555", "#55cc55", "#cdcd55", "#5455cb", "#cc55cc", "#7acaca", "#cccccc", "#555555", "#ff5555", "#55ff55", "#ffff55", "#5555ff", "#ff55ff", "#55ffff", "#ffffff"]` |  | The 16 ANSI colours, black to bright white |
| `selectionBackground` | `""` |  | Selection background; empty inverts the cell |
| `selectionForeground` | `""` |  | Selected text; empty inverts the cell |
| `theme` | `""` |  | A theme from ~/.config/afterglow/themes or the bundled ones (afterglow-dark, afterglow-light), or light:NAME,dark:NAME to follow the desktop. Its colours replace the ones below. Reads afterglow JSON, Ghostty and kitty theme files |
| `unfocusedDim` | `0.19` | 0–1 | Darkening over unfocused splits |

## Control

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `allowReadText` | `false` |  | Let them read what panes show, scrollback included |
| `enabled` | `true` |  | Let other programs control afterglow over D-Bus (afterglow ctl) |

## Font

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `antialiasing` | `"grayscale"` | `"grayscale"`, `"none"`, `"rgb"`, `"bgr"` | Font antialiasing: grayscale, none, rgb or bgr. Subpixel modes use grayscale with transparency or shaders |
| `boldFamily` | `""` |  | Bold font family; empty uses family |
| `boldItalicFamily` | `""` |  | Bold italic font family; empty uses the italic family |
| `boldWeight` | `700` | 100–900 | Weight of bold text (variable fonts) |
| `cellHeight` | `1` | 0.5–3 | Row height against the font's own: 1.2 adds a fifth of a line between rows |
| `cellWidth` | `1` | 0.5–3 | Column width against the font's own |
| `fallbackFamilies` | `[]` |  | Families tried, in order, for characters the font lacks (symbols, CJK), before the system's choice |
| `family` | `"Monaspace Neon Var"` |  | Regular text font family |
| `features` | `["calt", "liga", "ss01", "ss02", "ss03", "ss04", "ss05", "ss06", "ss07", "ss08", "ss09", "ss10"]` |  | OpenType features in HarfBuzz syntax: calt, liga, ss01, cv01=2, -liga |
| `gamma` | `1` | 0.5–3 | Glyph coverage gamma: 1 preserves coverage; higher values strengthen edges |
| `hinting` | `"light"` | `"none"`, `"light"`, `"full"` | Glyph hinting: none (as designed), light (vertical only, the default) or full (crispest) |
| `italicFamily` | `""` |  | Italic font family; empty uses family |
| `italicSlant` | `-11` | -90–90 | Slant of italic text (variable fonts) |
| `size` | `11` | 4–96 | Size in points |
| `symbolMap` | `{}` |  | Codepoint ranges drawn from a given family first, e.g. { "U+E000-U+F8FF,U+F0000-U+FFFFD": "Symbols Nerd Font Mono" }. Box drawing and powerline shapes are drawn by afterglow itself |
| `weight` | `400` | 100–900 | Weight of regular text (variable fonts) |
| `width` | `100` | 50–200 | Width axis (Monaspace: 100-125) |

## Hints

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `patterns` | `[{"action": "copy", "pattern": "(?:~?/\|\\.{1,2}/)?[\\w.@+-]+(?:/[\\w.@+-]+)*(?:\\.[\\w+-]+\|:\\d+)(?::\\d+){0,2}"}]` |  | Up to 16 regex hints with copy, open or command action; command is an argv list |

## Keybinds

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `bindings` | [default bindings](#default-bindings) |  | Action to key combination, or a list of them; an empty string unbinds |
| `leader` | `""` |  | Prefix key used by Leader sequences |
| `sequenceTimeout` | `1000` | 100–10000 | Milliseconds before an incomplete sequence is cancelled |
| `tableTimeout` | `5000` | 100–60000 | Idle milliseconds before leaving a key table |
| `tables` | `{}` |  | Named action-to-key maps, activated with table:name |

## Links

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `detectUrls` | `true` |  | Find URLs in plain text, besides the links programs mark (OSC 8) |
| `modifier` | `"ctrl"` | `"ctrl"`, `"shift"`, `"alt"`, `"none"` | Key held to hover and open links: ctrl, shift, alt, or none (a plain click opens them) |
| `openWith` | `""` |  | Command to open links with, the URL added at the end; empty uses the desktop's browser |

## Mouse

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `middleClick` | `"primary"` | `"primary"`, `"clipboard"`, `"none"` | Middle-click pastes: primary (the last selection), clipboard, or none |
| `rightClick` | `"menu"` | `"menu"`, `"paste"`, `"copy-or-paste"`, `"extend"` | Right-click: menu, paste, copy-or-paste (copy a selection, else paste) or extend (the selection) |

## QuickTerminal

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `animate` | `true` |  | Slide in and out |
| `height` | `0.4` | 0.1–1 | Height as a fraction of the screen |
| `shortcut` | `"Meta+&#96;"` |  | Global shortcut that shows and hides it; empty for none |

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
| `ambiguousWide` | `false` |  | East Asian Ambiguous characters (Greek, Cyrillic, some symbols) take two cells, for CJK setups |
| `bell` | `"attention"` | `"none"`, `"attention"`, `"visual"`, `"both"` | none, attention (mark the window urgent), visual (flash the pane) or both |
| `bellSound` | `""` |  | Sound for the bell: empty for none, default for the desktop's bell, or a sound file |
| `bidi` | `"off"` | `"off"`, `"auto"`, `"ltr"`, `"rtl"` | Bidirectional paragraph layout: off, auto, ltr or rtl; application grids keep logical order |
| `clipboardRead` | `"ask"` | `"ask"`, `"allow"`, `"deny"` | Programs reading the clipboard (OSC 52): ask, allow or deny |
| `confirmClose` | `true` |  | Ask before closing a split, tab or window where a program is running |
| `confirmUnsafePaste` | `true` |  | Ask before pasting several lines into a program that would run them |
| `copyOnSelect` | `"primary"` | `"primary"`, `"clipboard"`, `"off"` | Copy a selection when it is made: primary (middle-click paste), clipboard (both), or off |
| `cursorBlink` | `true` |  | Blink the cursor |
| `cursorBlinkInterval` | `530` | 100–5000 | Milliseconds the blinking cursor stays on, and off |
| `cursorShape` | `"block"` | `"block"`, `"underline"`, `"bar"` | block, underline or bar |
| `env` | `{}` |  | Variables added to the shell's environment, e.g. { "EDITOR": "nvim" } |
| `hideMouseWhileTyping` | `false` |  | Hide the mouse pointer while typing; moving it shows it again |
| `notifications` | `true` |  | Show desktop notifications programs send (OSC 9, OSC 777) |
| `pager` | `"less -R"` |  | Command used to view the last command output; no shell expansion |
| `scrollLines` | `3` | 1–50 | Lines a mouse wheel notch scrolls |
| `scrollOnInput` | `true` |  | Typing and pasting jump to the bottom |
| `scrollOnOutput` | `false` |  | New output jumps a pane scrolled back through history to the bottom |
| `scrollback` | `10000` | 0–1000000 | Lines of history per pane |
| `shell` | `""` |  | Program to run; empty runs your login shell |
| `shellIntegration` | `true` |  | Automatic Bash, Zsh and Fish prompt integration |
| `term` | `"xterm-256color"` |  | TERM for child programs |
| `wordSeparators` | `"&#96;\"'│\|:;,()[]{}<>"` |  | Characters that end a word for double-click selection, besides spaces |

## Window

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `backgroundOpacity` | `1` | 0.05–1 | Opacity of the default background; text stays opaque |
| `balancePadding` | `false` |  | Share the space left over by whole cells between opposite edges, instead of leaving it right and below |
| `blur` | `false` |  | Blur what shows through a translucent background (KWin) |
| `decorations` | `true` |  | The compositor's title bar and borders (new windows) |
| `initialColumns` | `100` | 10–1000 | Columns of a new window |
| `initialRows` | `30` | 4–500 | Rows of a new window |
| `padding` | `6` | 0–200 | Space around the grid in logical pixels |
| `paneTitles` | `false` |  | Label each split with its title while a tab has several |
| `startMode` | `"normal"` | `"normal"`, `"maximized"`, `"fullscreen"` | How new windows open: normal, maximized or fullscreen |
| `tabBar` | `"auto"` | `"auto"`, `"always"`, `"never"` | Show the tab bar: auto (while there are several tabs), always or never |
| `workingDirectory` | `"inherit"` |  | Where new tabs, splits and windows start: inherit (the focused pane's directory, or the launcher's), home, or a path. afterglow -d overrides it |

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
