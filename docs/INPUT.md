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
| `script:<id>.<action>` | Runs a script action |
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
Tables and sequences accept built-in, text and script actions.

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

The command palette, `ctl action` and Luau can also run these actions.
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
`Window.scrollbar` selects `auto`, `always` or `never`. `Window.scrollbarHideDelay` sets the fade delay in milliseconds.

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
The output limit is 1 MiB. A notice identifies truncation.
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
A pane content change, a scroll of the pane or a change of the layout cancels the hints.

## Links and URL hints

Afterglow opens OSC 8 links and plain-text URLs with a click while you hold the link modifier.

| Setting or action | Default | Function |
| --- | --- | --- |
| `Links.modifier` | `ctrl` | Key to hold for link highlighting and clicks. `none` opens links with a plain click. |
| `Links.detectUrls` | `true` | Finds plain-text URLs in addition to OSC 8 links |
| `Links.openWith` | empty | Command that receives the URL. Empty uses the desktop browser. |
| `Links.schemes` | `http`, `https`, `mailto`, `ftp`, `file` | Schemes that open without a prompt. Other schemes ask first. |
| `Links.hintAlphabet` | `asdfjklghqweruiopzxcvbnmty` | Letters that label hints |
| `show_urls_open` | Ctrl+Shift+G | Labels the URLs on screen and opens the selected URL |
| `show_urls_copy` | Ctrl+Shift+Y | Labels the URLs on screen and copies the selected URL |

## Mouse

| Setting | Default | Choices |
| --- | --- | --- |
| `Mouse.rightClick` | `menu` | `menu`, `paste`, `copy-or-paste`, `extend` |
| `Mouse.middleClick` | `primary` | `primary`, `clipboard`, `none` |

`copy-or-paste` copies a selection, or pastes when there is no selection.
`extend` extends the selection to the click.

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
`Terminal.confirmUnsafePaste` asks before a paste of several lines into a program that would run them.

## Clipboard requests

Programs read the clipboard with OSC 52 and OSC 5522. `Terminal.clipboardRead` sets the policy: `ask`, `allow` or `deny`.
With `ask`, a prompt names the tab of the program. The window stays on the current tab.
The prompt starts on Deny, so a Return typed for the shell denies the request.
An OSC 5522 request with a name and a password also offers **Always allow**.
An `a` key alone chooses it. A held or modified `a` does not.

## Keyboard protocols

modifyOtherKeys follows xterm.
At level 2, Shift with a letter sends the shifted letter, and Shift+Space is encoded.
Return, Tab, Backspace and Escape with modifiers are encoded as xterm encodes them.

In the kitty keyboard protocol, Afterglow reports Caps Lock and Num Lock only in the "report all keys" mode.
The kitty specification also reports them in the disambiguate mode for keys without text.
Afterglow does not, so that a program reads Num Lock+Up as Up.

With Num Lock off, the keypad keys stay keypad keys.
The kitty protocol reports them as the keypad's Left, Home, Begin and other keys.
Legacy encodings send them as the main arrow, Home, End, Page Up, Page Down, Insert and Delete keys.
The keypad 5 key (Begin) sends `CSI E`.
The decimal key of a layout with a decimal comma is the keypad separator.
F13 to F35 send the sequences of xterm's terminfo entry, and the kitty protocol codes 57376 to 57398.

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
You authenticate once when `XDG_RUNTIME_DIR` is private to you, or when your SSH configuration sets `ControlMaster auto`.
Afterglow records the host in `$XDG_STATE_HOME/afterglow/ssh-terminfo`, by default under `~/.local/state`.
The file holds only hashes. A host without `tic` is recorded as a fallback host.
Afterglow does not install the entry on that host again until the entry changes.
To install the entry again, delete the file.
If the host has no `tic`, or the local `infocmp` cannot read the entry, the session uses `xterm-256color`.
`Terminal.term=afterglow` also needs the entry on the local system. The build installs it only when it finds `tic`.
Set `Terminal.sshTerminfo` to `fallback` to always use `xterm-256color` on remote hosts and install nothing.

`ssh -G`, `ssh -V`, `ssh -Q` and `ssh -O` reach `ssh` unchanged.
Runs with `-N`, `-W` or `-f` do not install the entry or change the pane label.
An existing `ssh` function or alias takes precedence over the integration.
Set `Terminal.sshIntegration=false` to run `ssh` directly.
