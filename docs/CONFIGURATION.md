<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Configuration reference

`tools/generate-reference.py` generates this document from the runtime Phosphor schema. Group and key names are case-sensitive. See [appearance](APPEARANCE.md) for configuration loading and examples.

Afterglow clamps a number to the listed range. The description of Font.size gives 0 or less a meaning of its own. An integer key drops the fraction of a number, so 6.7 is 6. It uses its default for a value outside the 32-bit range and for text that is not a whole number. Invalid enum choices use the default. Where an empty string has a special meaning, the description says so. Omitted values use defaults.

A color in `Colors` has one of the forms of [color values](APPEARANCE.md#color-values), such as `#1e1e2e`, `red` or `oklch(0.7 0.1 200)`.

## Animation

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `bellCurve` | `"spring:14,1"` |  | Curve of the visual bell fade, in the splitCurve format. |
| `bellDuration` | `150` | 16–5000 | Duration of the visual bell fade in milliseconds for curves other than springs. |
| `cursor` | `true` |  | Animate cursor movement between cells. |
| `cursorCurve` | `"spring:48,1"` |  | Curve of cursor motion between cells, in the splitCurve format. |
| `cursorDuration` | `150` | 16–5000 | Duration of cursor motion in milliseconds for curves other than springs. |
| `overlayCurve` | `"0.4,0,0.2,1"` |  | Curve of the fade of an overlay over its pane (Shaders.overlayTransition), in the splitCurve format. The fade stops the first time that it reaches its target. A curve that overshoots does not bounce back. |
| `overlayDuration` | `180` | 16–5000 | Duration of the fade of an overlay (Shaders.overlayTransition) in milliseconds for curves other than springs. |
| `overviewCurve` | `"spring:12,0.82"` |  | Curve of the pane overview motion, in the splitCurve format. The default spring overshoots slightly. |
| `overviewDuration` | `450` | 16–5000 | Duration of pane overview motion in milliseconds for curves other than springs. |
| `paneEffectDuration` | `420` | 16–5000 | Milliseconds that a pane takes to open or close in Shaders.paneEffects. The portal of a pane whose shell moves to another host, user or container takes the same time. |
| `scroll` | `true` |  | Animate scrolling between rows. |
| `scrollCurve` | `"spring:26,1"` |  | Curve of scroll motion, in the splitCurve format. |
| `scrollDuration` | `150` | 16–5000 | Duration of scroll motion in milliseconds for curves other than springs. |
| `splitCurve` | `"spring:22,1"` |  | Curve of split animations, as a Phosphor curve spec. Use a spring such as spring:22,1 (angular frequency, damping ratio), a cubic Bezier such as 0.2,0,0,1, or a named easing such as elastic-out. An invalid spec uses the default. |
| `splitDuration` | `250` | 16–5000 | Duration of split animations in milliseconds for curves other than springs, which settle by themselves. |
| `splits` | `true` |  | Animate split opening, closing and resizing. It also controls the open and close animations, the portal and the away frame of Shaders.paneEffects. |
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
| `inputSource` | `"auto"` |  | Playback source for capture. The auto value, or an empty value, follows the default output. |
| `lowerCutoff` | `50` | 20–500 | Lowest analyzed frequency in hertz. |
| `smoothing` | `65` | 0–100 | Noise reduction of the spectrum analyzer. A value of 0 is fast and jumpy. 100 is slow and smooth. |

## Colors

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `background` | `"#1c1c1c"` |  | Default background color. An empty value uses the default. |
| `boldIsBright` | `false` |  | Use bright colors 8–15 for bold text with colors 0–7. |
| `cursor` | `""` |  | Cursor color. An empty value uses the foreground. |
| `cursorText` | `""` |  | Text color under a block cursor. An empty value uses the background. |
| `divider` | `""` |  | Color of the dividers between panes. An empty value uses the color a fifth of the way from the background color to the text color. A theme replaces it. A theme without a divider color gets the color a fifth of the way from its background color to its text color. |
| `foreground` | `"#d8d8d8"` |  | Default text color. An empty value uses the default. |
| `minimumContrast` | `1` | 1–21 | Minimum contrast ratio between text and its background. Afterglow changes the color of text with less contrast. A value of 1 keeps every color. A value of 4.5 is the WCAG AA level for normal text. |
| `palette` | `["#1c1c1c", "#cc5555", "#55cc55", "#cdcd55", "#5455cb", "#cc55cc", "#7acaca", "#cccccc", "#555555", "#ff5555", "#55ff55", "#ffff55", "#5555ff", "#ff55ff", "#55ffff", "#ffffff"]` |  | The 16 ANSI colors, from black to bright white. With fewer than 16, the others use the default. With more, Afterglow uses the first 16. An entry that is not a color uses the default. |
| `popupBorder` | `""` |  | Color of the border around a popup overlay. An empty value uses the color halfway from the background color to the text color. A theme replaces it. A theme without a popup border color gets the color halfway from its background color to its text color. |
| `popupDim` | `0.35` | 0–1 | Opacity of a black layer over the pane around a popup overlay. The popup and its border stay as they are. A value of 0 turns the darkening off. |
| `searchCurrentMatch` | `"#ff8c42"` |  | Background color of the current search match. An empty value uses the default. |
| `searchMatch` | `"#e5c07b"` |  | Background color of search matches. An empty value uses the default. |
| `searchMatchText` | `"#101010"` |  | Text color of search matches. An empty value uses the default. |
| `selectionBackground` | `""` |  | Selection background color. An empty value reverses the cell colors. |
| `selectionForeground` | `""` |  | Selected text color. An empty value reverses the cell colors. |
| `theme` | `""` |  | Theme name or file path. An empty value uses no theme. Afterglow searches the themes directory beside the config file (~/.config/afterglow/themes), then afterglow/themes in each XDG data directory. Then it searches the bundled themes, such as nord and afterglow-light, and the themes of Ghostty, Kitty and Konsole. Use light:NAME,dark:NAME to follow the desktop. Use plasma:NAME for the background, text, selection and cursor colors of the desktop color scheme over theme NAME. Use plasma alone for these colors over afterglow-dark or afterglow-light, after the brightness of the color scheme. A mux session runs without the desktop. It takes the dark NAME, and plasma takes only the colors that the color scheme file sets. A theme replaces each color and the visual bell intensity that it sets. It also replaces the divider and popup border colors. Afterglow reads its own JSON themes, Ghostty themes, Kitty themes and Konsole color schemes. A value with a slash is a file path. A path can start with ~/ for the home directory. A relative path starts at the directory of the config file. |
| `unfocusedDim` | `0.19` | 0–1 | Opacity of a black layer over unfocused panes. A value of 0 turns the darkening off. |
| `visualBell` | `"#ffffff"` |  | Color of the visual bell flash. An empty value uses the default. A color that a program sets with OSC 21 replaces it. |
| `visualBellIntensity` | `0.18` | 0–1 | Opacity of the visual bell flash when it starts. A value of 0 hides the flash. A theme that sets it replaces it. |

## Control

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `allowReadText` | `false` |  | Allow control clients, such as afterglow ctl, to read pane text, including scrollback, the commands that run and the values of user variables. It also allows screenshots of panes, tabs and windows. A script command that runs for a client, such as aglow run or afterglow run, reads pane text only when Scripts.allowReadText is on too. |
| `allowRemoteReadText` | `false` |  | Allow a script command that runs from a remote host, with aglow run in an afterglow ssh session, to read pane text. Any process of the remote user, and the remote root user, can run such a command and read its output. Scripts.allowReadText and Control.allowReadText must be on too. While this is off, only the handler of the run writes to its output. |
| `enabled` | `true` |  | Allow other programs, such as afterglow ctl and aglow run, to control Afterglow through its socket. The aglow run command also needs Scripts.enabled. Later launches of Afterglow use the socket also when this is off. A value that is not true or false turns it off. |
| `eventInterval` | `100` | 0–10000 | Shortest time in milliseconds between two events of one kind for one pane, for titles, status, context, bells and each user variable. A newer event replaces one that waits, and the bells in between become one. A value of 0 sends every event. |

## Font

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `antialiasing` | `"grayscale"` | `"grayscale"`, `"none"`, `"rgb"`, `"bgr"` | How Afterglow smooths glyph edges. The rgb and bgr values use grayscale while transparency or shaders are active. Pane effects and overlay fades count as shaders. Pane effects also run while a pane shows a shell on another host, as another user or in a container. |
| `boldFamily` | `""` |  | Font family of bold text. An empty value uses family. |
| `boldItalicFamily` | `""` |  | Font family of bold italic text. An empty value uses the italic family. |
| `boldWeight` | `700` | 100–900 | Weight of bold text. It selects a static font and sets the weight axis of a variable font. |
| `cellHeight` | `1` | 0.5–3 | Row-height multiplier relative to font metrics. A value of 1.2 adds 20 percent to the row height. |
| `cellWidth` | `1` | 0.5–3 | Column-width multiplier relative to font metrics. |
| `fallbackFamilies` | `[]` |  | Fallback families in priority order. These supply missing characters before system fallback. |
| `family` | `""` |  | Font family of regular text. An empty value uses the desktop's fixed-width font. |
| `features` | `["calt", "liga"]` |  | OpenType features in HarfBuzz syntax, such as calt, liga, ss01, cv01=2 or -liga. |
| `gamma` | `1` | 0.5–3 | Glyph coverage gamma. A value of 1 preserves coverage. Higher values increase edge coverage. |
| `hinting` | `"light"` | `"none"`, `"light"`, `"full"` | How glyph outlines align to the pixel grid. The none value keeps the outlines as designed. The light value aligns vertical positions only. The full value aligns both axes. |
| `italicFamily` | `""` |  | Font family of italic text. An empty value uses family. |
| `italicSlant` | `-11` | -90–90 | Slant of italic text in degrees, for variable fonts with a slant axis. |
| `size` | `0` | 4–96 | Size in points. A value of 0 or less uses the size of the desktop's fixed-width font. Afterglow clamps other values to 4–96. |
| `sizeStep` | `1` | 0.1–12 | Points that the font_bigger and font_smaller actions add or remove. |
| `symbolMap` | `{}` |  | Preferred families for codepoint ranges, such as { "U+E000-U+F8FF,U+F0000-U+FFFFD": "Symbols Nerd Font Mono" }. Afterglow draws box drawing, block elements, Braille and the Powerline separators U+E0B0 to U+E0BF itself. It also draws the block shapes of Symbols for Legacy Computing. This setting does not change them. |
| `weight` | `400` | 100–900 | Weight of regular text. It selects a static font and sets the weight axis of a variable font. |
| `width` | `100` | 50–200 | Width axis of variable fonts. A value of 100 is the normal width. Each font limits it to its own range. |

## Graphics

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `quota` | `320` | 16–4096 | Mebibytes of images (Kitty graphics, sixel and iTerm2) that each screen of a pane keeps, at 4 bytes for each pixel. A pane has a main and an alternate screen. When a screen is full, images without a placement go first: those sent with the transient hint (N=1), then the oldest. |

## Hints

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `alphabet` | `"asdfjklghqweruiopzxcvbnmty"` |  | Letters that label hints and URL hints, the easiest to reach first. It needs two or more letters, each one time. Other values use the default. |
| `patterns` | `[{"action": "copy", "pattern": "(?:~?/\|\\.{1,2}/)?[\\w.@+-]+(?:/[\\w.@+-]+)*(?:\\.[\\w+-]+\|:\\d+)(?::\\d+){0,2}"}]` |  | Up to 16 hints. Each is an object with a regular expression in pattern and an action: copy, open or command. An entry without an action copies. A command action also needs command, a list of 1 to 32 arguments. The first argument names the program. |

## Keybinds

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `bindings` | [default bindings](#default-bindings) |  | Map each action to a key combination or a list of them. An empty string removes the binding. Besides actions, the map can bind script:ID.COMMAND to run a script command, text:BYTES to send text and table:NAME to open a key table. |
| `leader` | `""` |  | Key that Leader stands for in sequences. With an empty value, a binding that uses Leader is an error. |
| `sequenceTimeout` | `1000` | 100–10000 | Milliseconds before Afterglow cancels an incomplete sequence. |
| `tableTimeout` | `5000` | 100–60000 | Idle milliseconds before leaving a key table. |
| `tables` | `{}` |  | Named key tables, each a map of actions to keys as in bindings. A binding to table:NAME opens one. Its actions keep it open. The pop_key_table action leaves every open table, and so does Keybinds.tableTimeout. |

## Links

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `detectUrls` | `true` |  | Detect plain-text URLs in addition to application-provided OSC 8 links. |
| `modifier` | `"ctrl"` | `"ctrl"`, `"shift"`, `"alt"`, `"none"` | Modifier for link highlighting and activation. With none, a plain click opens the link. |
| `openWith` | `""` |  | Command for opening links. Afterglow splits it into words, adds the URL as the last argument and runs it without a shell. Quotes keep spaces in one word. An empty value uses the desktop browser. |
| `schemes` | `["http", "https", "mailto", "ftp", "file"]` |  | URL schemes that open without a prompt. Links with other schemes ask first. |

## Mouse

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `autoScrollInterval` | `50` | 10–1000 | Milliseconds between the rows that a selection scrolls while you extend it past the top or bottom of a pane. |
| `dragSelection` | `false` |  | Allow a single press without Alt inside the selection to drag its text to another pane or another desktop application. A click there without a move clears the selection, and other presses there select as they do elsewhere. |
| `middleClick` | `"primary"` | `"primary"`, `"clipboard"`, `"none"` | What a middle click pastes. The none value turns middle-click paste off. |
| `momentumScroll` | `0.96` | 0–1 | Part of the speed of a finger scroll that the scroll keeps every 8 milliseconds after the last finger lifts. It applies to touchpads and touchscreens. A higher value scrolls further. A value of 0 stops the scroll when the finger lifts. A value of 1 does not slow the scroll, which goes on until a click, a key or a touch stops it. |
| `rightClick` | `"menu"` | `"menu"`, `"paste"`, `"copy-or-paste"`, `"extend"` | What a right click does. The menu value opens the context menu. The paste value pastes the clipboard. The copy-or-paste value copies an existing selection, or else pastes. The extend value extends the selection. |
| `touchScrollMultiplier` | `1` | -10–10 | Multiplier for the distance of a finger scroll on a touchpad or a touchscreen. A negative value scrolls the other way. A program that reads the mouse gets the direction only. |
| `touchSelectHold` | `400` | 100–2000 | Milliseconds that a finger holds still on a touchscreen before a move selects text. A finger that moves sooner scrolls. |

## Mux

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `eventQueue` | `4` | 1–64 | Most mebibytes of requests, such as bells, notifications and clipboard requests, that wait while the window of a session is slow. The session keeps one queue of this size, and each pane keeps another. A request that does not fit gets the answer of a session that no window shows. A request that finds the queue of its pane empty always fits. A change applies when Afterglow next attaches to a session. |
| `onClose` | `"kill"` | `"kill"`, `"detach"` | What closing the window of a mux session, or its last tab or pane, does. With kill, the session and its programs end, as in a window outside a session. With detach, the session keeps running with its tabs, and afterglow mux attach shows it again. Closing another tab or pane of the window ends its programs. The session also keeps running after afterglow mux detach, a takeover by another window, a quit of Afterglow, a crash and a lost connection. The window of a project that opens into a session always keeps its session. |
| `updateEnvironment` | `["DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "SSH_AUTH_SOCK", "SSH_AGENT_PID", "SSH_CONNECTION", "SSH_ASKPASS", "KRB5CCNAME"]` |  | Environment variables that a session of this computer takes from Afterglow each time a window shows it, such as a new SSH_AUTH_SOCK after you log in again. Afterglow's value replaces the session's, and a variable that Afterglow does not have leaves the session. Only the panes that start after that get the new values. A session of another host keeps its own. Names that start with AFTERGLOW_ are ignored. |

## Notifications

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `enabled` | `true` |  | Show desktop notifications that programs send with OSC 9, OSC 99 and OSC 777, and program status notifications (OSC 7501). Afterglow's own notifications show also when this is off. |
| `rateLimit` | `20` | 0–1000 | Desktop notifications that the programs of one pane can show in Notifications.rateWindow seconds. Afterglow drops the notifications over the limit. A value of 0 removes the limit. Updates of a shown notification do not count. The notifications of Afterglow itself do not count, such as those of afterglow ctl notify and of the notify effect of scripts. The notification at the end of a script command run without a client counts against the limit of its pane. Such notifications of runs without a pane share one limit. |
| `rateWindow` | `10` | 1–3600 | Seconds over which Notifications.rateLimit counts the notifications of one pane. |
| `statusInterval` | `3000` | 0–600000 | Shortest time in milliseconds between two program status notifications (OSC 7501) of one pane. A value of 0 shows each one. |

## QuickTerminal

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `animate` | `true` |  | Animate quick-terminal opening and closing. |
| `curve` | `"spring:22,1"` |  | Curve of the quick terminal slide, in the Animation.splitCurve format. |
| `duration` | `150` | 16–5000 | Duration of the quick terminal slide in milliseconds for curves other than springs. |
| `height` | `0.4` | 0.1–1 | Height as a fraction of the screen. |
| `position` | `"top"` | `"top"`, `"bottom"` | Screen edge the quick terminal slides from. |
| `screen` | `""` |  | Screen that shows the quick terminal, by its output name such as DP-1. An empty or unknown name uses the primary screen. |
| `shortcut` | `` "Meta+`" `` |  | Global shortcut to show or hide the quick terminal. An empty value turns the shortcut off. A value that is not a key combination uses the default. |

## Scripts

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `allowReadText` | `true` |  | Allow Luau scripts to read pane text, selections and semantic regions, and to run actions that read pane text. In a script command that runs for a client, such as aglow run or afterglow run, read_text, read_region and semantic_regions also need Control.allowReadText. |
| `commandOutputLimit` | `16384` | 64–262144 | Kibibytes of UTF-8 text that one run of a script command can write to standard output and standard error together. A run that writes more ends, and aglow run exits with status 1. A run from a key, a menu or the palette counts only its standard error. |
| `commandRunLimit` | `16` | 1–16 | Runs of the commands of one script at the same time. A run over the limit fails, and aglow run exits with status 1. Each run that waits uses one of the 16 waiting handlers of its script. |
| `commandStdinLimit` | `1024` | 0–4096 | Kibibytes of standard input that a script command can get from aglow run. A run with more input fails, and aglow run exits with status 2. A value of 0 refuses any input that is not empty. |
| `commandTimeout` | `600000` | 1000–3600000 | Longest time in milliseconds that one run of a script command can take. This includes runs from a key, a menu or the palette. The time that the run waits for a task, a dialog, an overlay or a slow reader of its output counts. The wait for standard input before the run starts does not count. At the limit the run ends, and aglow run exits with status 124. A change applies to the runs that start after it. |
| `cpuBudget` | `250` | 10–1000 | Milliseconds of handler time that one script can use in each second. Past it, Afterglow drops the script's events, and delays its results, timers, keys and command runs. Each second in which a call of the script waits counts. After 5 such seconds, each less than 3 seconds after the one before, Afterglow disables the script. Dropped events do not count. |
| `enabled` | `true` |  | Run Luau scripts from the scripts directory beside the config file and from afterglow/scripts in each XDG data directory. The aglow run command also needs Control.enabled. A value that is not true or false turns it off. |

## Shaders

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `background` | `[]` |  | Shaders run over each pane's background layer alone: cell backgrounds and images under the text. |
| `cursor` | `[]` |  | Shaders run over each pane's cursor layer alone. |
| `overlayTransition` | `"overlay-fade"` |  | Shader that blends a pane and the overlay that runs over it, as the overlay opens and closes. An empty value, or a pack that does not load, switches at once. |
| `overview` | `"pane-deck"` |  | Shader that draws the pane overview (pane_overview). An empty value turns the overview off. |
| `pane` | `[]` |  | Shaders run over each pane. Name a pack by its directory name, which is normally its id, such as cursor-trail or bloom. Name a pack of a package as <package>.<pack>, such as my-extension.wall. |
| `paneEffects` | `"pane-effects"` |  | Shader that animates panes as they split open and close. It draws a portal when the shell of a pane moves to another host, user or container. While the shell stays there, it draws the away frame around the pane. Animation.splits off turns these off too. An empty value turns it off. |
| `params` | `{}` |  | Parameter values for each shader reference, such as { "bloom": { "strength": 0.5 } }. The value of each reference must be an object. |
| `tabTransition` | `"tab-switch"` |  | Shader that animates switching tabs. An empty value switches at once. |
| `text` | `[]` |  | Shaders run over each pane's text layer alone: glyphs, decorations and images over the text. |
| `window` | `[]` |  | Shaders run over the whole window, across panes, after the pane shaders. |

## Terminal

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `allowScreenReadback` | `false` |  | Answer DECRQCRA checksum requests. These let any program in the pane read the screen back. |
| `ambiguousWide` | `false` |  | Use two cells for East Asian Ambiguous characters, including Greek, Cyrillic and some symbols. |
| `bell` | `"attention"` | `"none"`, `"attention"`, `"visual"`, `"both"` | Response to the bell. The none value does nothing. The attention value marks the window urgent while it is not active. The visual value flashes the pane. The both value does both. Each value but none also plays Terminal.bellSound. |
| `bellSound` | `""` |  | Bell sound. An empty value turns the bell sound off, and so does Terminal.bell none. The value "default" plays the bell of the desktop sound theme with canberra-gtk-play. Any other value is a sound file that pw-play plays, or paplay when pw-play is missing. Give an absolute path or a path that starts with ~/. |
| `bellSoundInterval` | `250` | 0–10000 | Shortest time in milliseconds between two bell sounds. Bells in between make no sound. A value of 0 plays every bell. |
| `bidi` | `"off"` | `"off"`, `"auto"`, `"ltr"`, `"rtl"` | Bidirectional layout of the primary screen. The off value keeps text in logical order. The auto value finds the direction of each paragraph. The ltr and rtl values set it. Application grids keep logical order. |
| `clipboardRead` | `"ask"` | `"allow"`, `"ask"`, `"deny"` | Programs reading the clipboard (OSC 52, OSC 5522). The ask value shows a prompt for each read. |
| `clipboardWrite` | `"allow"` | `"allow"`, `"ask"`, `"deny"` | Programs writing the clipboard (OSC 52, OSC 5522). The ask value shows a prompt for each write. |
| `confirmClose` | `true` |  | Ask before closing a pane, tab or window where a program is running. The command afterglow mux kill asks too, before it ends a session where a program is running. |
| `confirmUnsafePaste` | `true` |  | Ask before pasting or dropping text that holds a line break, unless the program uses bracketed paste. A line break could run a command. |
| `copyOnSelect` | `"primary"` | `"primary"`, `"clipboard"`, `"off"` | Where Afterglow copies a new selection. The primary value copies it to the primary selection. The clipboard value copies it to the clipboard too. The off value does not copy it. |
| `cursorBlink` | `true` |  | Make the cursor blink. |
| `cursorBlinkInterval` | `530` | 100–5000 | Duration in milliseconds for each visible and hidden cursor phase. |
| `cursorShape` | `"block"` | `"block"`, `"underline"`, `"bar"` | Shape of the cursor until a program sets another one with DECSCUSR. |
| `dragAnswerWait` | `100` | 0–1000 | Milliseconds that Afterglow waits for the answer of a program that uses Kitty drag and drop. It waits when a drag over the pane shows the program new types. The window does not draw while it waits. A value of 0 does not wait. |
| `dragTreeLifetime` | `10` | 1–1440 | Minutes that Afterglow keeps the files of a Kitty drag from another host after the drop. A drop target such as a file manager can copy them after the drag ends. A change applies to the drops that come after it. |
| `env` | `{}` |  | Variables added to the shell's environment, such as { "EDITOR": "nvim" }. Each value must be a string. A variable here replaces the value that Afterglow gives, such as COLORTERM. Afterglow ignores TERM, AFTERGLOW_SSH_HELPER and AFTERGLOW_SSH_TERMINFO here. Terminal.term, Terminal.sshIntegration and Terminal.sshTerminfo set them. Afterglow also ignores AFTERGLOW_PANE_ID, AFTERGLOW_SOCKET, AFTERGLOW_OVERLAY, AFTERGLOW_SESSION and AFTERGLOW_RESULT, which it sets or removes for each pane. New panes and new mux sessions use this value. |
| `fileTransferDeltaLimit` | `64` | 0–1048576 | Gibibytes that a Kitty delta or compressed file transfer can write when the program does not give the file size. A larger file fails with EFBIG. A value of 0 sets no limit. |
| `fileTransferPassword` | `""` |  | Shared secret for Kitty file-transfer approval. An empty value always asks for approval. |
| `graphicsFileTransfer` | `"allow"` | `"allow"`, `"deny"` | Images that programs send with the Kitty graphics protocol as a file, a temporary file or shared memory. Images sent inline always work. |
| `hideMouseWhileTyping` | `false` |  | Hide the pointer while typing. Pointer movement shows it again. |
| `lastOutputLimit` | `1024` | 1–65536 | Kibibytes of the last command output that the pager gets. For longer output, the pager gets the start and a note. |
| `pager` | `"less -R"` |  | Command for viewing the last command output. Afterglow splits it into words and runs it without a shell. An empty value turns the pager off. |
| `pasteReadTimeout` | `10` | 1–600 | Seconds after a paste in which the program can read the clipboard with the password of the paste event (OSC 5522). |
| `scrollLines` | `3` | 1–50 | Number of rows scrolled for each mouse-wheel notch. |
| `scrollOnInput` | `true` |  | Scroll to the bottom on typing or paste. |
| `scrollOnOutput` | `false` |  | Scroll to the bottom when new output arrives. |
| `scrollback` | `10000` | 0–1000000 | Lines of history that each pane keeps. A value of 0 keeps no history. |
| `shell` | `""` |  | Program that new panes and new mux sessions run, as a name or a path. It gets no arguments. An empty value starts your login shell. |
| `shellIntegration` | `true` |  | Integrate Bash, Zsh and Fish prompts in new panes and new mux sessions automatically. |
| `sshIntegration` | `true` |  | Run ssh through afterglow ssh in integrated shells. The pane label then shows the host, and the session offers COLORTERM and TERM_PROGRAM to the server. New panes and new mux sessions use this value. |
| `sshRemoteCommands` | `false` |  | Allow the script commands that are marked remote to run from aglow run on the hosts that afterglow ssh connects to. Every process of the remote user, and the remote root user, can then run those commands. It needs Control.enabled and Scripts.enabled. A change to on applies to the ssh sessions that start after it. A change to off closes the sockets of the running sessions. |
| `sshTerminfo` | `"install"` | `"install"`, `"fallback"` | What to do when TERM is afterglow: install its terminfo entry on each ssh host, or use xterm-256color there instead. New panes and new mux sessions use this value. |
| `term` | `"xterm-256color"` |  | Value of TERM for the programs in the panes. An empty value uses the default. New panes and new mux sessions use this value. |
| `textBlinkInterval` | `500` | 100–5000 | Duration in milliseconds for each visible and hidden phase of blinking text (SGR 5). |
| `wordSeparators` | `` "`\"'│\|:;,()[]{}<>" `` |  | Characters that end a word for double-click selection, besides spaces. With an empty value, only spaces end a word. |

## Window

| Key | Default | Range or choices | Meaning |
| --- | --- | --- | --- |
| `backgroundOpacity` | `1` | 0.05–1 | Opacity of the default background. Text stays opaque. |
| `balancePadding` | `false` |  | Distribute unused cell space between opposite edges. Otherwise, space remains at the right and bottom. |
| `blur` | `false` |  | Blur content behind translucent backgrounds with KWin. |
| `decorations` | `true` |  | Show the compositor's title bar and borders on new windows. |
| `initialColumns` | `100` | 10–1000 | Columns of a new window, and of a session that afterglow mux new starts. |
| `initialRows` | `30` | 4–500 | Rows of a new window, and of a session that afterglow mux new starts. |
| `padding` | `6` | 0–200 | Space around the grid in logical pixels. |
| `paneTitles` | `false` |  | Show pane titles in tabs with two or more panes. |
| `popupCorners` | `"rounded"` | `"rounded"`, `"square"` | Corners of the border around a popup overlay. The rounded value curves each corner. The square value makes each corner a right angle. |
| `scrollbar` | `"auto"` | `"auto"`, `"always"`, `"never"` | When the history scrollbar shows. The auto value shows the scrollbar while the view scrolls or the pointer is over it, then fades it out. |
| `scrollbarHideDelay` | `1200` | 0–60000 | Milliseconds an auto scrollbar stays visible after the last scroll or after the pointer leaves. A value of 0 starts the fade at once. |
| `scrollbarWidth` | `12` | 4–48 | Width of the history scrollbar, in logical pixels. |
| `startMode` | `"normal"` | `"normal"`, `"maximized"`, `"fullscreen"` | How new windows open. The normal value opens them at Window.initialColumns and Window.initialRows. The maximized value fills the screen beside the panels. The fullscreen value covers the whole screen. |
| `tabBar` | `"auto"` | `"auto"`, `"always"`, `"never"` | When the tab bar shows. The auto value shows it while there are two or more tabs. |
| `tabBarHeight` | `30` | 20–64 | Height of the tab bar, in logical pixels. |
| `workingDirectory` | `"inherit"` |  | Starting directory for new tabs, panes and windows. Use inherit, home or a path. The inherit value, or an empty value, uses the focused pane's directory or the launcher's directory. A path must be absolute, ~, or start with ~/. A path that is not a directory uses inherit. The afterglow -d option overrides this setting. A session of afterglow mux new starts in its --cwd or in the current directory. |

## Default bindings

| Action | Keys |
| --- | --- |
| `cancel_overlay` | `"Ctrl+Shift+Alt+Escape"` |
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
