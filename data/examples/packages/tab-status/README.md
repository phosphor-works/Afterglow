<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Tab and repository status

This package shows directory, Git status, command, exit result and remote context in native tab and status controls.
It uses ideas from [tabline.wez](https://github.com/michaelbrusegard/tabline.wez).

Install from the repository root:

```bash
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/tab-status ~/.config/afterglow/scripts/
```

When you use a custom configuration file, use the `scripts/` directory beside that file.
The active pane of each tab adds a suffix to the tab title.
The focused pane also sets the status label.
Run a command, change directory or switch panes to see changes.
**Toggle tab and repository status** enables or disables the display.
The choice survives restart. The keybind token is `script:tab-status.toggle`.

## Configuration

Create `config.json` beside `package.json` to change the defaults:

```json
{
  "segments": ["cwd", "git", "command", "exit", "context"],
  "separator": "  |  ",
  "tab_title": true,
  "git": true,
  "git_refresh_seconds": 15
}
```

| Component | Content |
| --- | --- |
| `cwd` | Directory basename. |
| `git` | Branch and `*` for tracked changes. Untracked files are excluded. |
| `command` | Running shell-integrated command, or `idle`. |
| `exit` | `ok` or `exit N` after command completion. |
| `context` | Remote or root identity supplied by the shell. |

| Key | Default | Description |
| --- | --- | --- |
| `segments` | All five components | Component names in display order. Only the first eight names are read. Unknown names among them are ignored. |
| `separator` | `"  \|  "` | Text between components. At most 16 bytes are used. Control characters become spaces. |
| `tab_title` | `true` | Adds the directory basename and the running command to the tab title. |
| `git` | `true` | Shows the `git` component. `false` stops all Git queries. |
| `git_refresh_seconds` | `15` | Time that a Git result stays cached. The range is 5 to 300 seconds. A value outside the range is clamped. |

Command and exit components require shell integration.

## Refresh and limits

The focused pane refreshes each second.
The other tabs refresh in rotation, one tab each second.
Git runs asynchronously without shell interpolation or optional index locks.
Git runs with `core.fsmonitor` turned off, so a repository cannot start its file monitor command.
Git still reads the configuration of each repository.
Set `git` to `false` before you open directories that you do not trust.
Each query has a two-second timeout and a 16 KiB output limit.
Results remain cached for 15 seconds by default.
Command completion invalidates that directory's cache.
Missing Git, non-repositories and failed or oversized queries omit the Git component.
Remote-context panes do not query Git for the local directory.
The package does not perform network Git operations.

Text uses the system theme through Qt controls.
The package does not add component colors, icons, powerline shapes or custom tab widgets.
These features require additional UI contribution interfaces.
Tab suffixes and status text from several packages add to each other.
Use one package that sets them.

Remove `scripts/tab-status/` to uninstall.
The host removes the timer, Git job and UI contributions.
`state/scripts/tab-status.json` beside the configuration retains the toggle state.
