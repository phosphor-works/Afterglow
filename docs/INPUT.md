<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Keyboard and terminal input

This page tells users how to bind keys and how Afterglow handles input.
It covers the keyboard, the mouse, touch input, selection, search, hints, links, paste, the clipboard and shell integration.

## Keybinds

`Keybinds.bindings` maps an action to one key combination or a list of combinations.
An empty string removes the binding.
The [configuration reference](CONFIGURATION.md#default-bindings) lists the default bindings.

| Action form | Function |
| --- | --- |
| Built-in action name | Runs a terminal action |
| `text:<bytes>` | Sends bytes to the terminal |
| `script:<id>.<command>` | Starts a script command with no arguments (see [commands](SCRIPTING.md#commands)). Afterglow reports a form without the script ID or the command as an error. |
| `table:<name>` | Activates a key table |

Text bindings support `\e`, `\r`, `\n`, `\t`, `\\`, `\xHH` and `\uHHHH`.
For example, `"text:\\x1b\\r": "Shift+Return"` sends Escape and Return.

A sequence contains up to four comma-separated keys.
Afterglow rejects a sequence with more keys and logs an error.
`Leader` stands for the key in `Keybinds.leader`, in any letter case.
`Keybinds.leader` has no default. Set it before you use `Leader`.
`Leader` must be the first key of a sequence. It can also be a binding alone.
Afterglow rejects a binding that uses `Leader` when no leader is set.
Named key tables give temporary input modes.

```json
{
  "Keybinds": {
    "leader": "Ctrl+Space",
    "sequenceTimeout": 1000,
    "tableTimeout": 5000,
    "bindings": {
      "split_right": "Leader, E",
      "table:resize": "Leader, R"
    },
    "tables": {
      "resize": {
        "resize_left": "H",
        "resize_right": "L",
        "resize_up": "K",
        "resize_down": "J",
        "pop_key_table": "Return"
      }
    }
  }
}
```

The window shows the active sequence or table, and that Escape cancels it.
Escape, a timeout, a focus change and a configuration reload cancel the mode.
A table can bind Escape. Escape then runs that action instead.
Modifier keys, AltGr, Super, Caps Lock and Num Lock do not cancel the mode.
A key that does not match exits the mode. Afterglow then processes that key normally.
A held key that matches nothing in a table does not exit the table.
Afterglow does not send the prefix keys that a sequence used to the shell.
A matching action leaves a table active. An action can activate another table.
`pop_key_table` leaves every active table. It works only inside a key table.

| Timeout | Default | Result |
| --- | --- | --- |
| `Keybinds.sequenceTimeout` | 1000 ms | Cancels an incomplete sequence. An active table stays. |
| `Keybinds.tableTimeout` | 5000 ms | Leaves a table after this time without a key |

A key that the configuration binds takes that key from any default binding.
Afterglow reports two explicit bindings of one key as an error.
The first binding in alphabetical order of the action names keeps the key.
Afterglow rejects ambiguous sequences and prefixes that overlap single-key shortcuts.
Remove the original binding before you use its key as a prefix.
Tables and sequences accept built-in actions, text bindings and script commands.
A held key starts a script command once. Its repeats do not start it again.
A held key also runs these actions once:

- `new_tab`
- `new_window`
- `split_right`
- `split_down`
- `split_left`
- `split_up`
- `close_pane`
- `close_tab`
- `move_pane_to_tab`
- `move_pane_to_window`
- `detach_tab`
- `show_last_output`
- `show_first_output_on_screen`
- `show_last_visited_output`
- `open_config`
- `reload_config`
- `toggle_fullscreen`
- `toggle_pane_zoom`
- `notify_on_finish`

`toggle_fullscreen` does nothing in a quick terminal. There, its key goes to the program, with the repeats of a held key.
The other actions, such as focus, resize, scroll and font size, repeat while you hold the key.
A table or sequence that names a command that no script has shows a notification.
A single binding to such a command sends the key to the terminal.

A binding can name a symbol that needs Shift, such as `%` or `+`.
`Ctrl++` then matches Ctrl+Shift+= on a US layout.
Shift+Tab matches bindings that name `Shift+Tab`.
On a non-Latin layout, a key without Ctrl matches the key that the first Latin layout puts there.

### Remap modifiers

`Keybinds.remapModifiers` makes one modifier act as another in Afterglow, as `remap_modifiers` does in Kitty.
This example swaps Ctrl and Super:

```json
{"Keybinds": {"remapModifiers": "ctrl:super super:ctrl"}}
```

| Item | Description |
| --- | --- |
| Rule | A source modifier, a colon and the destination, such as `ctrl:super`. Spaces separate the rules. |
| Source | One modifier, which is `shift`, `ctrl`, `alt` or `super`. |
| Destination | One or more of the same modifiers, joined by `+`, such as `ctrl+shift` in `alt:ctrl+shift`. |
| Names | Kitty's other names also work: `control`, `opt`, `option`, `cmd`, `command`, `⇧`, `⌃`, `⌥` and `⌘`. Letter case has no effect. |
| Two rules for one source | The last rule applies. The rules apply together, so a swap does not chain. |
| Error | A rule without a colon, a missing name such as `ctrl:`, an unknown name, two modifiers before the colon, or a rule that makes a modifier into itself. Afterglow shows the problem and remaps no modifier. |

Afterglow remaps the modifiers of each key, mouse and wheel event before anything reads the event.
Afterglow also remaps a modifier that is latched or locked, for example with sticky keys.
These use the modifier that a key becomes:

- Key bindings, key tables and leader sequences.
- `Links.modifier`, and the Shift and Alt of [mouse selection](#mouse).
- The keys and mouse reports that programs get, and the keys that scripts get in `on_key`.
- The tab bar, the command palette and the prompts.

Write the bindings for the modifier that a key becomes, not the modifier on the key.
In a binding, the Super key is `Meta`, as in `Meta+Shift+T`.
With `ctrl:super super:ctrl`, the default `Ctrl+Shift` bindings move to the Super key.
With only `super:ctrl`, they work from the Ctrl key and from the Super key.
A rule without its opposite, such as `ctrl:super` alone, leaves no key for its source modifier.
The bindings on that modifier then do nothing, and so does a `Links.modifier` of that modifier.
In a script's key mode, `end_script_key_mode` and `cancel_overlay` also match the modifiers on the keys, so the mode always has a way out.
The command palette and the context menu show the bindings as you write them.

In the [Kitty keyboard protocol](#keyboard-protocols), a program that asks for all keys gets the press and release of each modifier key.
When the destination is one modifier, a Shift, Ctrl, Alt or Super key reports that modifier on the same side.
The key that types a character and the character itself come from the keyboard layout with the new modifiers.
Caps Lock and AltGr work as they do without a remap.

These keep the modifiers on the keys:

- The desktop and other applications. A desktop shortcut takes its key before Afterglow gets it.
- `QuickTerminal.shortcut`, which is a desktop shortcut.
- The questions before a file transfer, a download and an upload.
- The Save dialog of a download, and the file dialog of an upload.
- A drag and drop between applications.

A rule that names Kitty's `hyper` or `meta` is an error.
A Hyper or Meta key gives the modifier that the keyboard layout gives it, if any.
The remap applies to that modifier, and the key keeps its name.
In the Kitty keyboard protocol, a Meta key (`Meta_L` or `Meta_R`) reports the Meta key code of Kitty, not Alt.

## Pane controls

| Action | Default shortcut | Function |
| --- | --- | --- |
| `toggle_pane_zoom` | Ctrl+Shift+Return | Shows the focused pane across the tab |
| `equalize_splits` | Ctrl+Shift+Alt+= | Gives the splits of the tab equal sizes |
| `resize_left`, `resize_right` | Ctrl+Shift+Alt+Left/Right | Moves a vertical divider by `Window.resizeStepCells` cells (default 2) |
| `resize_up`, `resize_down` | Ctrl+Shift+Alt+Up/Down | Moves a horizontal divider by `Window.resizeStepCells` cells (default 2) |

The command palette, `afterglow ctl action` and Luau can also run these actions.
Zoom keeps the split ratios and pane IDs.
Directional focus changes the pane that zoom mode shows.
A split or resize action first restores the layout.
Hidden panes keep their previous terminal size and continue to run.

A split must have at least four columns and two rows, plus padding.
Splits animate when they open, close and resize. `Animation.splits` turns this off.
`Animation.splitCurve` takes a Phosphor curve spec, such as `spring:22,1` or `0.2,0,0,1`.
A spring settles by itself. Other curves take `Animation.splitDuration` milliseconds.
At an outside edge, resize moves the nearest divider on the selected axis.
See [panes and projects](PROJECTS.md) to move panes between windows.

## Keyboard selection

1. Press Ctrl+Shift+Space to enter `copy_mode`.
2. Move with arrow keys or hjkl.
3. Select a mode with v, Shift+V or Ctrl+V.
4. Press y or Return to copy the selection and exit.

The keys v, Shift+V and Ctrl+V select characters, lines and rectangles.
PageUp, PageDown, Home and End also move the cursor.
Ctrl+Home and Ctrl+End move to history boundaries.
Escape or q cancels the selection. The / key opens search.
Wide characters and combining sequences stay complete.
A resize cancels the selection because its coordinates can change.
The focused pane has a history scrollbar.
By default, it shows while the view scrolls or the pointer is over it, then fades out.
`Window.scrollbar` selects `auto`, `always` or `never`.
`Window.scrollbarHideDelay` sets the fade delay in milliseconds. A value of 0 starts the fade at once.
`Window.scrollbarWidth` sets the width of the bar in logical pixels.

## Search

The `.*` control turns on PCRE2 regular expressions.
Search joins soft-wrapped lines and maps results to terminal cells.
It includes wide characters and combining sequences.
Lowercase queries ignore case. A query with uppercase letters uses case-sensitive matching.
Smart case ignores regular-expression syntax, so an escape such as `\S` does not select case-sensitive matching.
A class that names a case, such as `\p{Lu}` or `[[:lower:]]`, selects case-sensitive matching.

Capture runs in small parts. Matching runs in a worker thread.

| Limit | Value | At the limit |
| --- | --- | --- |
| Query length | 4,096 characters | Search refuses the query and shows a notice |
| Text examined | 4,194,304 characters | Search examines no more text and shows a notice |
| Matches | 10,000 | Search returns the first matches and shows a notice |
| Search time | 500 ms | Search stops and shows a notice |

Search results describe the state at the capture.
Change the query or reopen search to include new output.
A resize during capture cancels the search.

## Command output

Shell integration marks the prompt, the command and the output of each command (OSC 133).
These actions open the output of a command in a new tab, as in Kitty:

| Action | Output |
| --- | --- |
| `show_last_output` | The last command |
| `show_first_output_on_screen` | The first command whose output shows in the pane |
| `show_last_visited_output` | The last command that `jump_prompt_up`, `jump_prompt_down` or `afterglow ctl scroll --prompt` went to. While `Mouse.commandOutputClick` is not `none`, a right-click with `Mouse.commandOutputModifier` (Ctrl+Shift by default) on a command also sets it. |

They have no default keys.
Press Ctrl+Shift and right-click a prompt, a command or its output to open the output of that command.
`Mouse.commandOutputModifier` sets the modifiers of this click.
Write one or more of `ctrl`, `shift`, `alt` and `super`, joined with `+`.
The click needs these modifiers and no others.
`Mouse.commandOutputClick` changes this click. `select` selects the output, and `none` makes it a plain right-click.

The actions use `Terminal.pager`, which defaults to `less -R`.
`Terminal.pagerOutputLimit` sets the output limit in KiB, from 1 to 65536. The default is 1024 (1 MiB).
A notice tells you when Afterglow cut the output.
The pager reads a temporary file. Afterglow removes the file when the pane closes.
In the window of a mux session, the pager runs in the session, and can keep running after the window closes.
There, the pager gets the file already open, as `/dev/fd/4`, and Afterglow removes the file before the pager starts.
The pager command uses arguments without shell expansion.
When shell markers are missing, a notification tells you.
If the pager is not installed, or Afterglow cannot show the output, a notification gives the reason.

## Text hints

Press Ctrl+Shift+H to run `show_hints`.
This action labels regular-expression matches in the viewport.
The default pattern finds file paths with the rules of [file path links](#file-paths).
The default action copies the selected match.

Set `Hints.patterns` to change this behavior:

```json
{
  "Hints": {
    "patterns": [
      {"pattern": "[A-Za-z0-9_./-]+:[0-9]+", "action": "copy"},
      {"pattern": "[A-Za-z0-9_./-]+:[0-9]+:[0-9]+", "action": "command",
       "command": ["kate", "--line", "{line}", "--column", "{column}", "{path}"]}
    ]
  }
}
```

| Action | Function |
| --- | --- |
| `copy` | Copies the matched text |
| `open` | Opens a file or directory as a click on a [file path link](#file-paths) does |
| `command` | Runs an explicit argument list without a shell |

Commands accept `{match}`, `{path}`, `{line}` and `{column}` placeholders.
`{line}` and `{column}` come from a trailing `:LINE` or `:LINE:COLUMN` in the match.
A missing line or column is 1. A leading `~/` is the home directory.
Relative paths use the pane working directory.
When that directory is not known, an action on a relative path does not run, and a notice says why.
In a pane of a mux session of another host, `open` and `command` do not run. The paths of the pane are on that host.
A command does not run when a placeholder makes an argument start with a dash.

| Limit | Value | At the limit |
| --- | --- | --- |
| Patterns | 16 | Afterglow ignores the patterns after the 16th and logs a warning |
| Command arguments | 1 to 32 | Afterglow ignores the pattern and logs a warning |
| Viewport text | 65,536 characters | Text past the limit gets no labels |
| Matches | 1,000 distinct matches | Afterglow labels the first matches only |

Afterglow also ignores a pattern without `pattern` or with an unknown action, and logs a warning.
Hint patterns are case-sensitive.
Type a label to select it. Press Escape to cancel.
`Hints.alphabet` sets the letters of the labels, the easiest to reach first.
It needs two or more letters, and each letter can occur only one time. Other values use the default.
A pane content change, a scroll of the pane or a change of the layout cancels the hints.
The same is true for the URL labels of `show_urls_open` and `show_urls_copy`.
In a pane with continuous output, the labels therefore close at the next output.
A pane can hold its last frame while a program updates it (synchronized output, mode 2026).
It also holds its last frame while the shell draws its prompt again after a change of the pane width.
URL labels that open then are on the text of that frame.
They stay until the hold ends, and close then.

## Links and URL hints

Afterglow opens OSC 8 links, plain-text URLs and file paths with a click while you hold the link modifier.

| Setting or action | Default | Function |
| --- | --- | --- |
| `Links.modifier` | `ctrl` | Key to hold for link highlighting and clicks: `ctrl`, `shift`, `alt` or `super`. `none` opens links with a plain click. |
| `Links.detectUrls` | `true` | Finds plain-text URLs in addition to OSC 8 links |
| `Links.detectPaths` | `true` | Finds file paths in the text. A path is a link only when the file or directory exists. |
| `Links.openWith` | empty | Command that receives the URL as its last argument. Afterglow splits it into words and runs it without a shell. Empty uses the desktop browser. |
| `Links.openFilesWith` | empty | Program and arguments that open a file path. Empty opens the path as a `file://` URL. |
| `Links.remoteClients` | `ssh`, `mosh-client`, `et`, `telnet`, `rlogin` | Programs that connect to another host. While one of them runs in a pane, the pane has no path links. |
| `Links.schemes` | `http`, `https`, `mailto`, `ftp`, `file` | Schemes that open without a prompt. Other schemes ask first. |
| `Hints.alphabet` | `asdfjklghqweruiopzxcvbnmty` | Letters that label hints and URL hints |
| `show_urls_open` | Ctrl+Shift+G | Labels the URLs on screen and opens the selected URL |
| `show_urls_copy` | Ctrl+Shift+Y | Labels the URLs on screen and copies the selected URL |

### File paths

Afterglow finds these paths in the text:

- Absolute paths, and paths that start with `~/`, `./` or `../`.
- Relative paths that contain a `/`, or that end in a file extension, such as `src/main.cpp` or `README.md`.
- A file name with a line number, such as `Makefile:12`.

Each path can end in `:LINE` or `:LINE:COLUMN`, as in compiler and grep output.
Punctuation at the end of a path, such as a full stop or a comma, is not part of the path.
A path in a URL or in an OSC 8 link is part of that link.

A relative path starts from the working directory of the pane, which the shell reports with OSC 7.
Without a report, Afterglow uses the directory of the foreground program.
In a pane of a mux session, a relative path is a link only when the shell reports its directory.
The program of a session pane runs in the mux daemon, and the window does not wait for the daemon on each pointer move.
`~/` is your home directory.

A path is a link only when the file or directory exists.
Afterglow does this check in the background, so a slow network mount does not stop the window.
The underline and the hand pointer show when the check is complete.
A click on a path before its check opens the path when the check finds it.

A pane whose shell runs on another host or in a container has no path links.
Its paths name files on that host.
Afterglow knows this from these items:

- The shell context (OSC 3008 and the iTerm2 `RemoteHost` sequence).
- An OSC 7 directory on another host.
- A pane of a mux session on another host.
- The foreground program of the pane, when its process name is in `Links.remoteClients`. This includes `afterglow ssh`, which runs as `ssh`.

A pane of a mux session on this computer uses the shell integration in place of the foreground program:

- At a prompt, the pane has path links.
- While a command runs, the pane has path links when the first word of the command line is not in `Links.remoteClients`. Variable assignments and the directory of the program are not part of this word, and `afterglow ssh` counts as `ssh`.
- While a command runs that the shell did not report, the pane has no path links.
- Without shell integration, the pane has no path links.

| Limit | Value | At the limit |
| --- | --- | --- |
| Result of a check | 5 seconds | Afterglow checks the path again at the next hover |
| Kept results | 1,024 paths | Afterglow removes the oldest result |
| Checks that wait or run | 64 | Afterglow checks no new paths until a check completes |
| Process name in `Links.remoteClients` | 15 characters | Linux keeps the first 15 characters of a process name. Afterglow compares these characters only. |

Afterglow does not find a remote program that runs inside a local program, such as `sudo ssh host`.
The foreground program is then `sudo`, and the paths in its output can be links to local files.
The same applies to the command line of a session pane.

Set `Links.openFilesWith` to open files in an editor at the line.
The list is the program and its arguments, without a shell.
Each argument can contain these placeholders:

| Placeholder | Value |
| --- | --- |
| `{file}` | The absolute path |
| `{line}` | The line of a `:LINE` suffix. The value is 1 when the path has no line. |
| `{column}` | The column of a `:LINE:COLUMN` suffix. The value is 1 when the path has no column. |

```json
{
  "Links": {
    "openFilesWith": ["kate", "--line", "{line}", "--column", "{column}", "{file}"]
  }
}
```

A directory always opens in the file manager.
With an empty `Links.openFilesWith`, a file opens as a `file://` URL.
It opens with `Links.openWith`, or else with the desktop application for the file type.
When `file` is not in `Links.schemes`, Afterglow asks before it opens a path.
In the context menu, Copy Link Address copies the absolute path, not a `file://` URL.

## Mouse

| Setting | Default | Range or choices |
| --- | --- | --- |
| `Mouse.rightClick` | `menu` | `menu`, `paste`, `copy-or-paste`, `extend` |
| `Mouse.middleClick` | `primary` | `primary`, `clipboard`, `none` |
| `Mouse.autoScrollInterval` | `50` | 10 to 1000 milliseconds |
| `Mouse.momentumScroll` | `0.96` | 0 to 1, the part of the speed that a finger scroll keeps every 8 milliseconds |
| `Mouse.touchScrollMultiplier` | `1` | -10 to 10 |
| `Mouse.touchSelectHold` | `400` | 100 to 2000 milliseconds |
| `Mouse.dragSelection` | `false` | `true`, `false` |
| `Mouse.clickMovesCursor` | `true` | `true`, `false`. See [click on the command line](#click-on-the-command-line). |
| `Mouse.commandOutputClick` | `pager` | `pager`, `select`, `none`. See [command output](#command-output). |
| `Mouse.commandOutputModifier` | `ctrl+shift` | One or more of `ctrl`, `shift`, `alt` and `super`, joined with `+`. See [command output](#command-output). |

`copy-or-paste` copies a selection, or pastes when there is no selection.
`extend` extends the selection to the click.

A selection that you extend past the top or bottom of a pane scrolls one row each `Mouse.autoScrollInterval` milliseconds.

When the program in a pane reports the mouse, hold Shift to select. The program does not get a press with Shift.

### Drag selected text out of a pane

Set `Mouse.dragSelection` to `true` to drag selected text to another pane or another desktop application.
The setting is off by default, as in Kitty.
While it is off, a press inside the selection starts a new selection.

1. Select text with the mouse.
2. Press the left button inside the selection.
3. Move the pointer to another pane or another desktop application.
4. Release the button.

The target gets the text, and the selection stays.
The drag starts when the pointer moves the system drag distance.
A pane that gets the text pastes it, unless its program accepts drops (see [text drops](DESKTOP.md#drop-files-and-text-into-a-pane)).
The pane that the text comes from does not paste it.
A pane pastes text that you drop on it also while `Mouse.dragSelection` is off.

Only a single press with the left button and no Alt can drag:

- A click inside the selection without a move clears the selection. A release on another cell is a short drag that selects nothing.
- To select again from a point inside the selection, click once to clear the selection, then drag.
  A press on the same cell within the double-click time is a double click, and its drag selects whole words.
- A double click or a triple click inside the selection selects a word or a line. An Alt press starts a block selection.
- A pen selects. A finger on a touchscreen selects after it holds still (see [Touchpads and touchscreens](#touchpads-and-touchscreens)). Neither drags the selection.
- When the program in the pane offers a drag of its own ([Kitty drag and drop](DESKTOP.md#kitty-drag-and-drop)), a move starts the program's drag.

### Touchpads and touchscreens

A scroll with fingers on a touchpad or a touchscreen goes on after the last finger lifts, and slows down until it stops.
Afterglow uses the rules of Kitty for the speed and the stop:

- The speed comes from the moves in the last 150 milliseconds and from the lift. Fingers that hold still before they lift make a slower scroll.
- A scroll of 10 logical pixels or less does not go on after the lift.
- Every 8 milliseconds, the scroll keeps the part of its speed that `Mouse.momentumScroll` sets.
- The scroll stops when its speed is less than half a pixel, or at the top or the bottom of the history.
- A value of 0 stops the scroll when the finger lifts. A value of 1 does not slow the scroll.

These events stop the scroll at once:

| Event | Note |
| --- | --- |
| A click or a touch | Also a touch of two fingers |
| A key press or a key release | |
| Another window takes the focus | |
| A scroll action, a jump to a prompt, URL hints or copy mode | Also from a script or `afterglow ctl` |
| Typing or output that moves the view | |
| The pane goes out of view, closes, or gets an overlay | Also when the overview opens |
| The program in the pane goes to the alternate screen, or back | |

A wheel turn does not stop the scroll. As in Kitty, the wheel scrolls in addition.
Fingers that rest on a touchpad do not stop the scroll, because Qt does not report a hold gesture.

`Mouse.touchScrollMultiplier` multiplies the distance of a finger scroll. A negative value scrolls the other way.

A touchscreen works with one finger, as in Kitty.

| Gesture | Result |
| --- | --- |
| Touch and lift within 8 logical pixels in each direction | A left click where the finger touched. Two taps are a double click. |
| Touch and move more than 8 logical pixels before `Mouse.touchSelectHold` | A scroll that follows the finger, and goes on after the finger lifts |
| Touch, hold still for `Mouse.touchSelectHold`, then move | A drag of the left button from where the finger touched, which selects |
| Touch with two fingers or more together, or with a second finger before the first moves | No click, scroll or selection |
| Touch with a second finger after the first moves | Afterglow ignores the second finger, and the first goes on |
| Touch the tab bar, the scrollbar, a dock or a divider | A press of the left button at once, so that a move drags |

The pointer does not show when a finger touches. Shader packs that read the pointer follow the finger.

In a program that reads the mouse, a tap is a click for the program.
A hold then a move is a drag for the program.
Hold Shift to select instead.

A finger scroll gives a program that reads the mouse one wheel report for each row of distance.
On the alternate screen with alternate scroll, a finger scroll gives one arrow key for each row of distance.
A wheel notch gives `Terminal.scrollLines` rows.

## Quick terminal

The quick terminal is a window that slides in from a screen edge.
Its global shortcut shows and hides it. `afterglow --quick-terminal` does the same.

| Setting | Default | Function |
| --- | --- | --- |
| `QuickTerminal.shortcut` | ``Meta+` `` | Global shortcut. Empty turns it off. |
| `QuickTerminal.height` | 0.4 | Height as a fraction of the screen |
| `QuickTerminal.position` | `top` | Screen edge, `top` or `bottom` |
| `QuickTerminal.screen` | empty | Output name, such as `DP-1`. Empty uses the primary screen. |
| `QuickTerminal.animate` | `true` | Slides the window in and out |

A quick terminal that shows stays on its output.
A change to `QuickTerminal.screen` applies after the quick terminal hides and shows again.

## Paste

Afterglow removes control characters from pasted text, in bracketed and plain paste mode.
It keeps tab. Each line end (a carriage return, a line feed, or both) goes as one carriage return, the Enter key.
It removes every other control character, also DEL and the C1 controls.
`Terminal.confirmUnsafePaste` asks before a paste with a line break (carriage return or line feed) into a program that does not use bracketed paste.
A paste into a program that does not use paste events (OSC 5522), and each drop of text that pastes, have this limit:

| Item | Limit | At the limit |
| --- | --- | --- |
| Text of one paste | 16 MiB | Afterglow pastes nothing and shows a notice. |

## Clipboard requests

Programs read the clipboard with OSC 52 and OSC 5522. `Terminal.clipboardRead` sets the policy, which is `ask`, `allow` or `deny`.
`Terminal.clipboardWrite` sets the policy of writes in the same way.
With `ask`, a prompt names the tab of the program. The window stays on the current tab.
The prompt starts on Deny, so a Return typed for the shell denies the request.
An OSC 5522 request with a name and a password also offers **Always allow**.
An `a` key alone chooses it. A held or modified `a` does not.
**Always allow** allows the later requests of the pane of the same kind that use the same password, as the Kitty specification says.
An answer to a read allows later reads, and an answer to a write allows later writes, as Kitty keeps them apart.
After a paste, the program can read the clipboard with the password of the paste event for `Terminal.pasteReadTimeout` seconds (10 by default).
This password allows reads only.

Clipboard requests and notifications of a pane wait for the window in one queue.

| Item | Limit | At the limit |
| --- | --- | --- |
| Requests and notifications that wait | 256 items, or 128 MiB of their data | A clipboard request gets `EBUSY`. The program can try again. Afterglow drops a notification. A query of the notifications on screen (OSC 99 `p=alive`) still gets an answer. A warning in the log gives the number of dropped items. |

## Keyboard protocols

modifyOtherKeys follows xterm.
At level 2, Shift with a letter sends the shifted letter, and Afterglow encodes Shift+Space.
Afterglow encodes Return, Tab, Backspace and Escape with modifiers as xterm encodes them.

In the Kitty keyboard protocol, Afterglow reports Caps Lock and Num Lock only in the "report all keys" mode.
The Kitty specification also reports them in the disambiguate mode for keys without text.
Afterglow does not, so that a program reads Num Lock+Up as Up.

With Num Lock off, the keypad keys stay keypad keys.
The Kitty protocol reports them as the keypad's Left, Home, Begin and other keys.
Legacy encodings send them as the main arrow, Home, End, Page Up, Page Down, Insert and Delete keys.
The keypad 5 key (Begin) sends `CSI E`.
The decimal key of a layout with a decimal comma is the keypad separator.
F13 to F35 send the sequences of xterm's terminfo entry, and the Kitty protocol codes 57376 to 57398.

Ctrl with a key of a non-Latin layout uses the key of the first Latin layout, as Qt's shortcuts do.
Ctrl+С on a Russian layout sends Ctrl+C in legacy modes and in the Kitty protocol.
When a program asks for alternate keys (flag 4), the Kitty protocol sends two keys, such as `CSI 1089::99;5u`. These are the key of the active layout and the base layout key.
The base layout key is the key in the US layout.
Afterglow sends it when it is not the same as the key.

The Kitty protocol reports AltGr and the fifth level key as ISO_Level3_Shift and ISO_Level5_Shift.
They set no modifier bit.
It also reports the media and volume keys, when the desktop does not take them first for its own shortcuts.
When you release one of two keys of a modifier, the event keeps the bit of the modifier. An example is the left Ctrl key while the right one is down.

## Shell integration

New panes automatically integrate interactive Bash 4.4 or later, Zsh and Fish.
Shell markers identify prompts, commands, exit status and working directories.
Command navigation and Luau events use these markers.

Bash and Zsh keep user startup files and prompt hooks.
A prompt that has its own OSC 133 or OSC 633 marks keeps them. Afterglow then adds no marks, also none to the secondary prompt.
The Zsh integration adds its marks after the prompt hooks of a theme, also after a hook that loads later.
Fish 4 marks its prompts itself, and Afterglow keeps that.
Afterglow does not change user startup files.
It supports login and interactive flags.
It does not inject integration into scripts, `-c` commands or explicit startup overrides.
Examples of startup overrides are `--norc`, `-f` and `--no-config`.

Set `Terminal.shellIntegration` to `"disabled"` to turn off automatic injection into new panes.
Afterglow reports `disabled` with other words as a problem of the configuration, and `disabled` applies.
To turn off only some parts, give the switches of Kitty's `shell_integration`, with spaces between them.
An example is `"no-title no-sudo"`.

| Switch | Turns off |
| --- | --- |
| `no-prompt-mark` | The prompt, command and exit-status marks (OSC 133), and the mark of the secondary prompt (`PS2`) in Bash and Zsh. In Fish 4, it also turns off the marks of Fish (`--features=no-mark-prompt`). With `no-rc`, Afterglow does not change the Fish command line, and Fish 4 keeps its own marks. |
| `no-cwd` | The working directory (OSC 7). Fish 4 sends its own. |
| `no-cursor` | The bar cursor while you edit the command line, and the configured cursor for the command. The bar blinks when `Terminal.cursorBlink` is on when the shell starts. The vi key bindings of Bash, Zsh and Fish keep their own cursors. |
| `no-title` | The title. This is the directory at the prompt, and the command while it runs, after the host name in an ssh login. Fish sets its own titles. |
| `no-sudo` | A `TERM` that every system knows for `sudo` and `run0`. With `Terminal.term` set to `afterglow`, `sudo` and `run0` drop the directory of Afterglow's terminfo entry. Thus the integration gives them `TERM=xterm-256color` in their environment, which `env_reset` of `sudo` keeps. This applies only when the system has no entry for `afterglow`. |
| `no-rc` | Nothing loads the integration. Afterglow sets only its environment, and your own startup files can load the integration. See [load the integration by hand](#load-the-integration-by-hand). |
| `no-complete` | Nothing. Afterglow accepts it, as Kitty does, and has no completions to turn off. |

A Fish older than 4 gets the prompt marks from Afterglow, with Kitty's special key for a click on the command line.

Alternatively, set `AFTERGLOW_SHELL_INTEGRATION` to `0` in `Terminal.env`.
The variable in the launch environment applies only when the launch starts a new process.
This does not remove hooks that you configured by hand, or native Fish markers.
Custom shell wrappers need their own integration.
Zsh integration does not load when `/etc/zsh/zshenv` sets `ZDOTDIR`.

Bash gets the title of a running command from its history.
A line that the history does not keep gets no title, with one exception.
With `HISTSIZE=0`, no command gets a title.

The exception is a command that repeats the last command while `HISTCONTROL` has `ignoredups` or `erasedups`.
The repeat gets its title, unless one of these conditions is true:

- `HISTCONTROL` has `ignorespace`. `ignoreboth` has `ignorespace` and `ignoredups`.
- `HISTIGNORE` is not empty.
- The history is off (`set +o history`).

The history then can drop lines that are not repeats, and Afterglow cannot tell the repeat from a dropped line.

### Load the integration by hand

Your own startup file can load the integration.
Do this with the `no-rc` switch, or for a shell that Afterglow did not start.
Add the line for your shell to its startup file:

| Shell | Startup file | Line |
| --- | --- | --- |
| Bash | `~/.bashrc` | `source /usr/share/phosphor-works/afterglow/shell-integration/bash` |
| Zsh | `~/.zshrc` | `source /usr/share/phosphor-works/afterglow/shell-integration/zsh/.zshenv` |
| Fish | `~/.config/fish/config.fish` | `source /usr/share/phosphor-works/afterglow/shell-integration/fish` |

`/usr/share` is the data directory of the install (`CMAKE_INSTALL_FULL_DATADIR`).
An install with another prefix has the scripts in its own data directory, such as `/usr/local/share`.
The integration loads one time in a shell.
When Afterglow loaded it first, the line in your startup file does nothing.

### Click on the command line

A click on the command line moves the cursor of the shell there, as in Kitty.
This works in a shell that marks its prompt (OSC 133 `A`), with no `click_events` needed.
The prompt mark must be at or above the cursor, and the cursor must show.
It does not work in the output, in the history, on the alternate screen, or while a program reads the mouse.
Set `Mouse.clickMovesCursor` to `false` to turn it off.

| The shell's prompt mark (OSC 133 `A`) | What a click sends |
| --- | --- |
| No `click_events` | Left or right arrow keys, one for each character between the cursor and the click |
| `special_key=1` | Kitty's key 1 in place of the arrows: `CSI 1 u` to the right, `CSI 1 ; 1 u` to the left. Kitty's Fish integration binds them. |
| `click_events=1` | An SGR mouse click (`CSI < 0 ; x ; y M`, then `m`) with the row of the screen as y |
| `click_events=2` | An SGR mouse click with y relative to the top line of the current prompt, from 0. The current prompt is the secondary prompt (`OSC 133 ; A ; k=s`) when it starts on or above the click. |

A secondary prompt (`OSC 133 ; A ; k=s`, the prompt of a continuation line) is not a new prompt.
The command that you typed before it stays.
A command line in `OSC 133 ; C ; cmdline=` can have the shell quoting of `printf %q`, with backslashes, quotes and `$'...'`.
Afterglow removes the quoting.

### SSH

Integrated shells run `ssh` through `afterglow ssh`.
The command accepts the same arguments as `ssh`.
While the session runs, the pane label shows the remote host.
The session offers `COLORTERM`, `TERM_PROGRAM` and `TERM_PROGRAM_VERSION` to the server.
The server accepts these variables only when its `AcceptEnv` setting allows them.
A `SetEnv` setting in your SSH configuration replaces this offer.

The default `TERM` is `xterm-256color`, and every host knows that entry.
If `Terminal.term` is `afterglow`, the first session to a host installs the `afterglow` terminfo entry in `~/.terminfo` on that host.
The install and the session share one connection.
You authenticate once when your SSH configuration sets `ControlMaster auto`.
Without it, you authenticate once when `XDG_RUNTIME_DIR` is an absolute path of letters, digits, `/`, `.`, `_` and `-`.
Afterglow puts its own control socket there, so keep that directory private to you, as systemd makes it.
For this first connection, Afterglow's shared connection replaces a `ControlPath none` setting for that host.
The reason is that `ssh -G` shows `none` and the default value in the same way.
To keep `ControlPath none`, set `Terminal.sshTerminfo` to `fallback`.
With `-M`, the session does not join a master that runs already, and the install does not become a master.
Thus the session authenticates on its own. When no master runs, you authenticate two times, once for the install and once for the session.
Afterglow records the host in `$XDG_STATE_HOME/phosphor-works/afterglow/ssh-terminfo`, by default under `~/.local/state`.
The file holds only hashes. Afterglow records a host without `tic` as a fallback host.
Afterglow does not install the entry on that host again until the entry changes.
To install the entry again, delete the file.
If the host has no `tic`, or the local `infocmp` cannot read the entry, the session uses `xterm-256color`.
`Terminal.term=afterglow` also needs the entry on the local system. The build installs it only when it finds `tic`.
Set `Terminal.sshTerminfo` to `fallback` to always use `xterm-256color` on remote hosts and install nothing.

With `Terminal.sshRemoteCommands` on, `aglow run` on the host can run the script commands that are marked `remote`, through `afterglow ssh`.
Before the session starts, `afterglow ssh` runs a probe over the shared connection and forwards a socket of Afterglow to the host.
With the setting on, every process of the remote user, and the remote root user, can run the commands that are marked `remote`.
Unlike the terminfo install, remote commands keep a `ControlPath none` or `ControlMaster no` of yours.
Afterglow reads your configuration files again to tell them from the defaults, and then forwards no socket for that session.
With `-v`, `afterglow ssh` tells why a session gets no socket.
See [remote commands](CONTROL.md#remote-commands).

`ssh -G`, `ssh -V`, `ssh -Q` and `ssh -O` reach `ssh` unchanged.
Runs with `-N`, `-W`, `-f` or `-s` do not install the entry, change the pane label or forward a socket for remote commands.
The same applies when the SSH configuration sets `SessionType none`, `SessionType subsystem` or `ForkAfterAuthentication yes`.
An existing `ssh` function or alias comes before the integration.
Set `Terminal.sshIntegration=false` to run `ssh` directly.
