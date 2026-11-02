<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Tab line

This package draws each window's tab bar as a tab line, in the terminal font.
It shows the project, the tabs, and the command, exit result, Git branch, directory and remote context of the focused pane.
It uses ideas from [tabline.wez](https://github.com/michaelbrusegard/tabline.wez).

Install from the repository root:

```bash
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/tab-status ~/.config/afterglow/scripts/
```

When you use a custom configuration file, use the `scripts/` directory beside that file.
The tab line takes one row at the top of each window, in place of the tab bar.
Click a tab to switch to it.
Run a command, change directory or switch tabs to see changes.
**Toggle the tab line** turns it off and on. Off, the windows show their usual tab bars.
The choice survives restart. The keybind token is `script:tab-status.toggle`.

## Layout

| Part | Content |
| --- | --- |
| Left | The window's project, when the window has one. |
| Middle | One block for each tab: its number and title. The current tab has the `active` colors. |
| Right | The components of the focused pane, against the right edge. The last one has the `accent` colors. |

| Component | Content |
| --- | --- |
| `command` | The running shell-integrated command. |
| `exit` | `ok` or `exit N` after a command completes. |
| `git` | Branch, or the commit on a detached `HEAD`. With `git_changes`, also `*` for tracked changes. Untracked files are excluded. |
| `cwd` | Directory basename. |
| `context` | Remote or root identity that the shell supplies. |

Command and exit components require shell integration.
An empty component does not show.

## Configuration

Create `config.json` beside `package.json` to change the defaults:

```json
{
  "segments": ["command", "exit", "git", "cwd", "context"],
  "powerline": true,
  "tab_max_length": 24,
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
| `tab_max_length` | `24` | Bytes of a tab title that show. The range is 4 to 80. |
| `colors` | See below | Colors for each kind of block, as `{"fg": color, "bg": color}`. A color is a palette index from 0 to 255 or `"#rrggbb"`. |
| `git` | `true` | Shows the `git` component. `false` stops all Git queries. |
| `git_changes` | `false` | Adds `*` when tracked files have changes. This runs `git status`, which can run commands from the repository's own configuration. |
| `git_refresh_seconds` | `15` | Time that a Git result stays cached. The range is 5 to 300 seconds. A value outside the range is clamped. |

| Block | Default `fg` | Default `bg` |
| --- | --- | --- |
| `section` (the project) | 0 | 4 |
| `active` (the current tab) | 0 | 4 |
| `inactive` (other tabs) | 7 | 8 |
| `component` | 15 | 8 |
| `accent` (the last component) | 0 | 6 |

Palette indices 0 to 15 are the colors of the theme, so the line follows a theme change.
A color that Afterglow does not accept leaves the window without a tab line. The script log shows the error.

## Refresh and limits

The line redraws when a tab, title, focus, command or context changes, and once each second.
An unchanged line is not sent again.
Git runs asynchronously without shell interpolation or optional index locks.
Git runs with `core.fsmonitor` turned off, so a repository cannot start its file monitor command.
By default, Git reads only `HEAD` to find the branch. It does not compare the work tree.
`git_changes` compares the work tree with `git status`.
That comparison runs the clean filters that the configuration of the repository names.
Programs can set the directory of a pane with an escape sequence, so a printed file can select any local directory.
Keep `git_changes` at `false` if a pane can show directories or output that you do not trust.
One query runs at a time, for the focused pane of each window in turn.
Each query has a two-second timeout and a 16 KiB output limit.
Results remain cached for 15 seconds by default.
Command completion invalidates that directory's cache.
Missing Git, non-repositories and failed or oversized queries omit the Git component.
Remote-context panes do not query Git for the local directory.
The package does not perform network Git operations.

Remove `scripts/tab-status/` to uninstall.
The host removes the timer, Git job and tab lines, and the tab bars come back.
`state/scripts/tab-status.json` beside the configuration retains the toggle state.
