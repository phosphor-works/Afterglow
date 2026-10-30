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

For a custom configuration, use the adjacent `scripts/` directory.
The active pane supplies each tab's title and status within a few seconds.
Run a command, change directory or switch panes to see changes.
**Toggle tab and repository status** enables or disables the display.
The choice survives restart. The keybind token is `script:tab-status.toggle`.

## Configuration

Create `config.json` beside `package.json` to change component order:

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

Command and exit components require shell integration.
Unknown component names are ignored. Up to eight components are displayed.

## Refresh and limits

The package refreshes one tab each second and rotates across all tabs.
Git runs asynchronously without shell interpolation or optional index locks.
Each query has a two-second timeout and a 16 KiB output limit.
Results remain cached for 15 seconds by default. The permitted range is 5–300 seconds.
Command completion invalidates that directory's cache.
Missing Git, non-repositories and failed or oversized queries omit the Git component.
Remote-context panes do not query local Git for reported directories.
The package does not perform network Git operations.

Text uses the system theme through Qt controls.
The package does not add component colors, icons, powerline shapes or custom tab widgets.
These features require additional UI contribution interfaces.
Avoid multiple packages that replace the same tab title or status.

Remove `scripts/tab-status/` to uninstall.
The host removes the timer, Git job and UI contributions.
`state/scripts/tab-status.json` beside the configuration retains the toggle state.
