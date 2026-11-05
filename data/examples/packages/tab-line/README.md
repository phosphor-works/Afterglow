<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Tab line

This package draws each window's tab bar as a tab line, in the terminal font.
It shows the project, the tabs, and the command, exit result, Git branch, directory and context of the focused pane.
It uses ideas from [tabline.wez](https://github.com/michaelbrusegard/tabline.wez).

Install from the repository root:

```bash
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/tab-line ~/.config/afterglow/scripts/
```

When you use a custom configuration file, use the `scripts/` directory beside that file.
The tab line takes one row at the top of each window, in place of the tab bar.
It also shows in a window with one tab, and when `Window.tabBar` is `never`.
Set `single_tab` to `false` to remove it from a window with one tab.
Click a tab to switch to it.
Middle-click a tab to close it. Right-click a tab to open its tab menu, which can change its title, move it, detach it or close it.
Click `+` after the tabs to open a new tab.
Turn the mouse wheel over the line to go to the previous or the next tab.
Drag a tab to move it. The tab takes the place of the tab under the pointer.
The `+N` blocks at the ends show the number of tabs that do not fit. Over a `+N` block, the tab takes the place of the nearest tab that does not show.
A drag does not detach a tab. Use the tab menu to detach it.
A tab or button under the pointer shows in the `hover` colors.
Run a command, change directory or switch tabs to see changes.
Choose **Toggle the tab line** in the command palette to turn the line off and on.
When the line is off, the windows show their usual tab bars.
The choice survives restart. Bind the command as `script:tab-line.toggle`, or run `afterglow run tab-line toggle`.

A middle click that closes a tab and a click on `+` are structural effects.
A script can open or close at most three tabs in ten seconds, together.
Past that limit, `+` and the middle click do nothing until the ten seconds end.

## Layout

| Part | Content |
| --- | --- |
| Left | The window's project, when the window has one. |
| Middle | One block for each tab: its number and title. The current tab has the `active` colors. A tab without a title shows `shell`. Then `+`, which opens a new tab. |
| Right | The components of the focused pane, against the right edge. The last one has the `accent` colors. |

| Component | Content |
| --- | --- |
| `command` | The running shell-integrated command. |
| `exit` | `ok` or `exit N` after a command completes. |
| `git` | Branch, or the commit on a detached `HEAD`. With `git_changes`, also `*` for tracked changes. Untracked files are excluded. |
| `cwd` | Directory basename. |
| `context` | The remote, elevated or container identity that the shell supplies. |

Command and exit components require shell integration.
An empty component does not show.

When the tabs do not fit in the window, the line shows the current tab and the tabs next to it.
`+N` on each side shows the number of tabs that are not shown. Click `+N` to switch to the nearest of them.
The line shows at most 48 tabs.
The line uses the width that Afterglow gives it, so it fits the window as the window changes size.

## Configuration

Create `config.json` beside `package.json` to change the defaults:

```json
{
  "segments": ["command", "exit", "git", "cwd", "context"],
  "powerline": true,
  "single_tab": true,
  "tab_max_length": 24,
  "new_tab_button": true,
  "colors": {"active": {"fg": 0, "bg": 4}},
  "git": true,
  "git_changes": false,
  "git_refresh_seconds": 15
}
```

| Key | Default | Description |
| --- | --- | --- |
| `segments` | All five components | Component names on the right, in display order. Only the first eight names are read. Unknown names among them are ignored. |
| `powerline` | `true` | Joins the blocks with Powerline triangles. Afterglow draws them, so no special font is necessary. `false` puts a space between blocks. |
| `single_tab` | `true` | Shows the tab line in a window with one tab. |
| `new_tab_button` | `true` | Shows `+` after the tabs. |
| `tab_max_length` | `24` | Bytes of a tab title that show. The range is 4 to 80. A value outside the range changes to the nearest limit. |
| `colors` | See below | Colors for each kind of block, as `{"fg": color, "bg": color}`. A color is a palette index from 0 to 255 or `"#rrggbb"`. A block without `fg` or `bg` uses the default for that color. |
| `git` | `true` | Shows the `git` component. `false` stops all Git queries. |
| `git_changes` | `false` | Adds `*` when tracked files have changes. This runs `git status`, which can run commands from the repository's own configuration. |
| `git_refresh_seconds` | `15` | Time that a Git result stays cached. The range is 5 to 300 seconds. A value outside the range changes to the nearest limit. |

| Block | Default `fg` | Default `bg` |
| --- | --- | --- |
| `section` (the project) | 0 | 4 |
| `active` (the current tab) | 0 | 4 |
| `inactive` (other tabs) | 7 | 8 |
| `component` | 15 | 8 |
| `accent` (the last component) | 0 | 6 |
| `hover` (a tab or button under the pointer, except the current tab) | 0 | 7 |

Palette indices 0 to 15 are the colors of the theme, so the line follows a theme change.
A color that Afterglow does not accept leaves the window without a tab line. `afterglow ctl scripts` shows the error.
A `hover` color that Afterglow does not accept refuses only the line with a hovered item. The line stays, without hover colors.

## Refresh and limits

The line redraws at once when a tab, title, focus, command or context changes, and when a tab moves.
More changes in the next 250 ms redraw together, so a title that changes quickly sends few lines.
The line also redraws at once when the pointer moves onto a tab or a button, and when its width changes.
After 25 of these redraws in one second, the next ones wait for the 250 ms redraw.
When the pointer moves off the tabs and buttons, the line redraws with the 250 ms redraw.
The line also redraws once each second.
An unchanged line is not sent again.
A line that Afterglow drops, for example at the effect limit, is sent again at the next redraw.
A tab move that Afterglow drops at the effect limit is not tried again until the pointer goes onto another tab.
A line that Afterglow refuses as invalid is sent again only when it changes.

| Text | At most | Longer text |
| --- | --- | --- |
| Project | 40 bytes | Cut |
| Tab title | `tab_max_length` bytes | Cut |
| Command, Git branch | 80 bytes | Cut |
| Directory, context | 60 bytes | Cut |

Control characters show as spaces.

Each window has at most one line on its way at a time. The next line waits for the result of that line.
Lines use the `dock` effect limit, so they do not use up the effect limit that Git queries and clicks need.
Each line gets a result, so lines also count toward the result limit.

Git runs asynchronously without shell interpolation or optional index locks.
Git runs with `core.fsmonitor` turned off, so a repository cannot start its file monitor command.
By default, Git reads only `HEAD` to find the branch. It does not compare the work tree.
`git_changes` compares the work tree with `git status`.
That comparison runs the clean filters that the configuration of the repository names.
Programs can set the directory of a pane with an escape sequence, so a printed file can select any local directory.
Keep `git_changes` at `false` if a pane can show directories or output that you do not trust.
One query runs at a time, for the focused pane of each window in turn.
Each query has a two-second timeout and a 16 KiB output limit.
The package stops a query that has no result after 5 seconds.
Results remain cached for 15 seconds by default.
Command completion invalidates that directory's cache.
Missing Git, non-repositories, failed queries and an oversized branch query omit the Git component.
When `git status` lists more changes than the output limit, the component still shows `*`.
A query that Afterglow does not start, for example at the effect limit, runs again at the next second.
Panes with a context do not query Git for the local directory.
The package does not perform network Git operations.

Remove `scripts/tab-line/` to uninstall.
Afterglow removes the timers, the Git job and the tab lines, and the tab bars come back.
`state/scripts/tab-line.json` beside the configuration retains the toggle state.
