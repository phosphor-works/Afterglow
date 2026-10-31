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
`Leader` uses the value of `Keybinds.leader`.
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

The window shows the active sequence or table.
Escape, timeout, focus changes and configuration reload cancel the mode.
A mismatched key exits the mode. Afterglow then processes that key normally.
Afterglow does not send consumed prefix keys to the shell.
A matching action leaves a table active. An action can activate another table.

Afterglow rejects ambiguous sequences and prefixes that overlap single-key shortcuts.
Remove the original binding before you use its key as a prefix.
Tables and sequences accept built-in, text and script actions.

## Pane controls

| Action | Default shortcut | Function |
| --- | --- | --- |
| `toggle_pane_zoom` | Ctrl+Shift+Return | Shows the focused pane across the tab |
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

Capture runs in small parts. Matching runs in a worker thread.
Search examines at most 4 Mi characters and returns at most 10,000 matches.
A notice identifies truncated results or regular-expression limits.
Search results describe a captured state.
Change the query or reopen search to include new output.
A resize during capture cancels the search.

## Command output

`show_last_output` opens marked command output in a new tab.
It uses `Terminal.pager`, which defaults to `less -R`.
The output limit is 1 MiB. A notice identifies truncation.
The pager reads a temporary file. Afterglow removes the file when the pane closes.
The pager command uses arguments without shell expansion.
Missing shell markers or an unavailable pager cause a notification.

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
Relative paths use the pane working directory.
Limits are 16 patterns, 64 Ki characters and 1,000 distinct matches.
Type a label to select it. Press Escape to cancel.
A pane content change cancels the hints. Separate URL hints remain available.

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
Alternatively, launch with `AFTERGLOW_SHELL_INTEGRATION=0`.
This does not remove manually configured hooks or native Fish markers.
Custom shell wrappers and older Fish versions require their own integration.
