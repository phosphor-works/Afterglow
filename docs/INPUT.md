<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Keyboard and terminal input

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
Named key tables supply temporary input modes.

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
Escape, timeout, focus changes and configuration reload cancel the mode.
A table can bind Escape. Escape then runs that action instead.
Modifier keys, AltGr, Super, Caps Lock and Num Lock do not cancel the mode.
A mismatched key exits the mode. Afterglow then processes that key normally.
A held key that matches nothing in a table does not exit the table.
Afterglow does not send consumed prefix keys to the shell.
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
A table or sequence that names a command no script has shows a notification.
A single binding to such a command sends the key to the terminal.

A binding can name a symbol that needs Shift, such as `%` or `+`.
`Ctrl++` then matches Ctrl+Shift+= on a US layout.
Shift+Tab matches bindings that name `Shift+Tab`.
On a non-Latin layout, a key without Ctrl matches the key that the first Latin layout puts there.

## Pane controls

| Action | Default shortcut | Function |
| --- | --- | --- |
| `toggle_pane_zoom` | Ctrl+Shift+Return | Shows the focused pane across the tab |
| `equalize_splits` | Ctrl+Shift+Alt+= | Gives the splits of the tab equal sizes |
| `resize_left`, `resize_right` | Ctrl+Shift+Alt+Left/Right | Moves a vertical divider by two cells |
| `resize_up`, `resize_down` | Ctrl+Shift+Alt+Up/Down | Moves a horizontal divider by two cells |

The command palette, `afterglow ctl action` and Luau can also run these actions.
Zoom keeps the split ratios and pane IDs.
Directional focus changes the pane shown in zoom mode.
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

The selection modes are characters, lines and rectangles, respectively.
PageUp, PageDown, Home and End also move the cursor.
Ctrl+Home and Ctrl+End move to history boundaries.
Escape or q cancels selection. The / key opens search.
Wide characters and combining sequences remain complete.
A resize cancels the selection because its coordinates can change.
The focused pane has a history scrollbar.
By default it appears while the view scrolls or the pointer is over it, then fades out.
`Window.scrollbar` selects `auto`, `always` or `never`. `Window.scrollbarHideDelay` sets the fade delay in milliseconds. A value of 0 starts the fade at once. `Window.scrollbarWidth` sets the width of the bar in logical pixels.

## Search

The `.*` control enables PCRE2 regular expressions.
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

Search results describe a captured state.
Change the query or reopen search to include new output.
A resize during capture cancels the search.

## Command output

`show_last_output` opens marked command output in a new tab.
It uses `Terminal.pager`, which defaults to `less -R`.
`Terminal.lastOutputLimit` sets the output limit in KiB, from 1 to 65536. The default is 1024 (1 MiB).
A notice identifies truncation.
The pager reads a temporary file. Afterglow removes the file when the pane closes.
The pager command uses arguments without shell expansion.
Missing shell markers cause a notification.
If the pager is not installed, or the output cannot be shown, a notification gives the reason.

## Text hints

Press Ctrl+Shift+H to run `show_hints`.
This action labels regular-expression matches in the viewport.
The default pattern finds file and line references.
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
| `open` | Opens a local file through the desktop |
| `command` | Runs an explicit argument list without a shell |

Commands accept `{match}`, `{path}`, `{line}` and `{column}` placeholders.
`{line}` and `{column}` come from a trailing `:LINE` or `:LINE:COLUMN` in the match.
A missing line or column is 1. A leading `~/` is the home directory.
Relative paths use the pane working directory.

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

Afterglow opens OSC 8 links and plain-text URLs with a click while you hold the link modifier.

| Setting or action | Default | Function |
| --- | --- | --- |
| `Links.modifier` | `ctrl` | Key to hold for link highlighting and clicks. `none` opens links with a plain click. |
| `Links.detectUrls` | `true` | Finds plain-text URLs in addition to OSC 8 links |
| `Links.openWith` | empty | Command that receives the URL as its last argument. Afterglow splits it into words and runs it without a shell. Empty uses the desktop browser. |
| `Links.schemes` | `http`, `https`, `mailto`, `ftp`, `file` | Schemes that open without a prompt. Other schemes ask first. |
| `Hints.alphabet` | `asdfjklghqweruiopzxcvbnmty` | Letters that label hints and URL hints |
| `show_urls_open` | Ctrl+Shift+G | Labels the URLs on screen and opens the selected URL |
| `show_urls_copy` | Ctrl+Shift+Y | Labels the URLs on screen and copies the selected URL |

## Mouse

| Setting | Default | Range or choices |
| --- | --- | --- |
| `Mouse.rightClick` | `menu` | `menu`, `paste`, `copy-or-paste`, `extend` |
| `Mouse.middleClick` | `primary` | `primary`, `clipboard`, `none` |
| `Mouse.autoScrollInterval` | `50` | 10 to 1000 milliseconds |
| `Mouse.dragSelection` | `false` | `true`, `false` |

`copy-or-paste` copies a selection, or pastes when there is no selection.
`extend` extends the selection to the click.

A selection that you extend past the top or bottom of a pane scrolls one row each `Mouse.autoScrollInterval`
milliseconds.

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
- A touch or a pen selects. It does not drag.
- When the program in the pane offers a drag of its own ([Kitty drag and drop](DESKTOP.md#kitty-drag-and-drop)), a move starts the program's drag.

## Quick terminal

The quick terminal is a window that slides in from a screen edge.
Its global shortcut shows and hides it. `afterglow --quick-terminal` does the same.

| Setting | Default | Function |
| --- | --- | --- |
| `QuickTerminal.shortcut` | ``Meta+` `` | Global shortcut. Empty disables it. |
| `QuickTerminal.height` | 0.4 | Height as a fraction of the screen |
| `QuickTerminal.position` | `top` | Screen edge, `top` or `bottom` |
| `QuickTerminal.screen` | empty | Output name, such as `DP-1`. Empty uses the primary screen. |
| `QuickTerminal.animate` | `true` | Slides the window in and out |

## Paste

Afterglow removes control characters from pasted text, in bracketed and plain paste mode.
It keeps tab, carriage return and line feed. It removes DEL and the C1 controls.
`Terminal.confirmUnsafePaste` asks before a paste with a line break (carriage return or line feed) into a program
that does not use bracketed paste.
A paste into a program that does not use paste events (OSC 5522), and each drop of text that pastes, have this limit:

| Item | Limit | At the limit |
| --- | --- | --- |
| Text of one paste | 16 MiB | Afterglow pastes nothing and shows a notice. |

## Clipboard requests

Programs read the clipboard with OSC 52 and OSC 5522. `Terminal.clipboardRead` sets the policy: `ask`, `allow` or `deny`.
With `ask`, a prompt names the tab of the program. The window stays on the current tab.
The prompt starts on Deny, so a Return typed for the shell denies the request.
An OSC 5522 request with a name and a password also offers **Always allow**.
An `a` key alone chooses it. A held or modified `a` does not.
After a paste, the program can read the clipboard with the password of the paste event for
`Terminal.pasteReadTimeout` seconds (10 by default).

## Keyboard protocols

modifyOtherKeys follows xterm.
At level 2, Shift with a letter sends the shifted letter, and Shift+Space is encoded.
Return, Tab, Backspace and Escape with modifiers are encoded as xterm encodes them.

In the Kitty keyboard protocol, Afterglow reports Caps Lock and Num Lock only in the "report all keys" mode.
The Kitty specification also reports them in the disambiguate mode for keys without text.
Afterglow does not, so that a program reads Num Lock+Up as Up.

With Num Lock off, the keypad keys stay keypad keys.
The Kitty protocol reports them as the keypad's Left, Home, Begin and other keys.
Legacy encodings send them as the main arrow, Home, End, Page Up, Page Down, Insert and Delete keys.
The keypad 5 key (Begin) sends `CSI E`.
The decimal key of a layout with a decimal comma is the keypad separator.
F13 to F35 send the sequences of xterm's terminfo entry, and the Kitty protocol codes 57376 to 57398.

## Shell integration

New panes automatically integrate interactive Bash 4.4 or later, Zsh and Fish 4 or later.
Shell markers identify prompts, commands, exit status and working directories.
Command navigation and Luau events use these markers.

Bash and Zsh keep user startup files and prompt hooks.
Fish keeps its native `mark-prompt` behavior.
Afterglow does not change user startup files.
It supports login and interactive flags.
It does not inject integration into scripts, `-c` commands or explicit startup overrides.
Examples of startup overrides are `--norc`, `-f` and `--no-config`.

Set `Terminal.shellIntegration=false` to disable automatic injection into new panes.
Alternatively, set `AFTERGLOW_SHELL_INTEGRATION` to `0` in `Terminal.env`.
The variable in the launch environment applies only when the launch starts a new process.
This does not remove manually configured hooks or native Fish markers.
Custom shell wrappers and older Fish versions require their own integration.
Zsh integration does not load when `/etc/zsh/zshenv` sets `ZDOTDIR`.

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
You authenticate once when your SSH configuration sets `ControlMaster auto`, or else when `XDG_RUNTIME_DIR` is an absolute path of letters, digits, `/`, `.`, `_` and `-`.
Afterglow puts its own control socket there, so keep that directory private to you, as systemd makes it.
For this first connection, Afterglow's shared connection replaces a `ControlPath none` setting for that host.
The reason is that `ssh -G` shows `none` and the default value in the same way.
To keep `ControlPath none`, set `Terminal.sshTerminfo` to `fallback`.
With `-M`, the session does not join a master that runs already, and the install does not become a master.
Thus the session authenticates on its own. When no master runs, you authenticate two times: for the install and for the session.
Afterglow records the host in `$XDG_STATE_HOME/afterglow/ssh-terminfo`, by default under `~/.local/state`.
The file holds only hashes. A host without `tic` is recorded as a fallback host.
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
An existing `ssh` function or alias takes precedence over the integration.
Set `Terminal.sshIntegration=false` to run `ssh` directly.
