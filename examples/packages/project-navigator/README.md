<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Project navigator

This Luau package finds project directories and lets you select one.
It uses ideas from [sessionizer.wezterm](https://github.com/mikkasendke/sessionizer.wezterm).
It uses native selectors, asynchronous jobs, project layouts and persistent state.

## Contents

| File | Function |
| --- | --- |
| `providers.luau` | Finds directories and parses the output of the providers |
| `history.luau` | Keeps the bounded state and its migrations |
| `init.luau` | Coordinates the commands and the correlated results |

The package uses only declared local imports and Afterglow effects.

## Install

Copy this complete directory into `~/.config/phosphor-works/afterglow/scripts/project-navigator/`.
When you use a custom configuration file, use the `scripts/` directory beside that file.
The load of the package does not run commands.

From the repository root, run these commands:

```bash
mkdir -p ~/.config/phosphor-works/afterglow/scripts
cp -r examples/packages/project-navigator ~/.config/phosphor-works/afterglow/scripts/
```

The destination must contain `scripts/project-navigator/package.json`, not `scripts/package.json`.
Afterglow reloads scripts automatically when they change.

## Use

Open the command palette and choose one of these commands:

| Palette command | Command | Action |
| --- | --- | --- |
| **Navigate projects** | `pick` | Searches running projects, saved layouts, favorites, recent projects and discovered directories. |
| **Toggle favorite project directory** | `favorite` | Pins or unpins the current project directory, or the working directory of the pane in an ordinary window. |
| **Previous project** | `previous` | Returns to the most recent different project. |

**Navigate projects** uses the window of a running project again.
For a new directory, choose a saved layout or a shell.
The next time, the picker offers the last-used layout first.
Every template pane starts in that directory.

Bind the commands as `script:project-navigator.pick`, `script:project-navigator.favorite` and
`script:project-navigator.previous`.

From a shell:

```sh
aglow run project-navigator open ~/Projects/site dev   # open a directory, with the layout dev
aglow run project-navigator list                       # favorite, recent and saved projects
find ~/src -maxdepth 1 -mindepth 1 -type d | aglow run project-navigator import
```

| Command | Action |
| --- | --- |
| `open DIR [LAYOUT]` | Opens `DIR`, relative to the shell's directory. Without `LAYOUT`, it uses the last layout of the directory. |
| `list` | Prints `KIND<TAB>VALUE` lines: `favorite`, `recent`, `recent-layout` and `layout`. |
| `import` | Adds each directory on stdin, one on each line, to the favorites. |
| `pick`, `favorite`, `previous` | The palette commands. |

Only a `LAYOUT` that you give becomes the layout of the directory for the next time.
When the directory is open already, Afterglow focuses its window and does not apply `LAYOUT`.
`DIR` can start with `~` or `~/`. Give the full path for the home of another user.
The package resolves symbolic links in directories with `realpath`, as Afterglow does.
It resolves the directories that it saves, the roots and the directories that `fd` and `zoxide` find.
As a result, one directory has one entry, also when the shell reached it through a link.
Without `realpath`, a directory keeps the path that the shell or the provider gave.

`list` leaves out a value that contains a tab or a line break, and says so on stderr.
`import` skips a line that starts with `~user`, and a line that names a path over 256 bytes.

| Exit status | Cause |
| --- | --- |
| 0 | The command completed. |
| 1 | The command failed, or `import` skipped lines. The message is on stderr. |
| 2 | Wrong arguments, or a `DIR` that cannot be a directory. |

A command from the palette or a key has no shell.
Its failure shows as a notification on the pane.

Finish or cancel the current operation before you start another.
Escape cancels a selector and starts no project.
When the run of a command ends while it waits, for example at Ctrl+C on `aglow run`, no selector opens after it.

## Configuration

Create `config.json` beside `package.json` to override the defaults:

```json
{
  "roots": ["~/Projects", "~/work"],
  "fd": true,
  "zoxide": true,
  "max_depth": 5
}
```

| Key | Default | Description |
| --- | --- | --- |
| `roots` | `["~/Projects"]` | Directories to offer and to search for Git repositories. A root is `~`, or starts with `/` or `~/`. The package ignores another root, such as `~user/src`. The package reads the first eight entries. |
| `fd` | `true` | `false` turns off the search for Git repositories with `fd`. |
| `zoxide` | `true` | `false` turns off the directories from `zoxide`. |
| `max_depth` | `5` | How many directory levels `fd` searches below each root, from 1 to 16. A value outside this range changes to the nearest limit. |

## Providers

`fd` finds Git repositories, including worktrees with `.git` files.
`zoxide query --list` supplies frequently used directories.
Both commands are optional.
The picker title names each provider that did not start, timed out or that the host refused, and why.
For example, `(unavailable: fd-1 timed out)` or `(unavailable: zoxide not started)`.
Configured roots, saved layouts, favorites and history stay available.
The providers run at the same time.
A provider that exits with an error still supplies the complete paths that it printed.
A provider that the host refuses supplies no paths.

## Limits

| Item | Limit | At the limit |
| --- | --- | --- |
| Roots | 8 | The package ignores the other entries of `roots`. |
| Provider run time | 5 seconds | The provider stops and supplies no paths. |
| Provider output | 128 KiB | The provider stops. It supplies the complete paths that it printed. |
| Choices in the picker | 400 | The picker does not show more choices. Favorites, recent projects, running projects and roots come first. |
| Favorites | 100 | A new favorite fails. `import` skips the other lines and exits with status 1. |
| Lines in one `import` | 61 | `import` reads the first 61 lines that are not empty. It does not read the other lines, and exits with status 1. |
| Path of a favorite or a layout choice | 256 bytes | The package does not save the path. |

## Saved state

Favorites, the last 50 projects and layout choices stay after reloads and restarts.
The package keeps layout choices for favorites and recent directories only.
The state of the package is in `state/scripts/project-navigator.json` beside the active configuration, in the
`project-navigator` scope.
Schema 2 includes a migration example from the arrays of favorite and recent paths of schema 1.
When the state conflicts, the package keeps the newer file and reports the failed save.
The next command reads the current state again.
A launch can succeed when the save of its history fails.
`afterglow ctl scripts` reports package load errors and the last-good instances that it keeps.

## Reuse a split layout across repositories

1. Open two or three terminal splits.
2. Choose **Save Project** in the command palette.
3. Name the layout `development`.
4. Choose **Navigate projects**.
5. Type part of a repository path.
   With `fd` installed, Git repositories below the configured roots appear automatically.
   You can use the roots without discovery tools.
6. Press Enter.
7. Choose `development` in the **Layout for …** picker.
   A new window opens with the saved split structure, and every pane starts in the chosen repository.
   The explicit commands that you saved with the layout run again.
   The layout does not record commands that you type into an interactive shell.
8. Choose **Toggle favorite project directory** in the new window.
9. Open the navigator again.
   The picker marks that directory with a star and `[running]`.
10. Select another repository.
11. Choose a layout for it.
12. Choose **Previous project**.
    Afterglow returns to the earlier directory.
    The selection of a running directory uses its window and running processes again.
13. Restart Afterglow.
    Favorites and recent directories stay in the picker.
    When you select a directory, the picker offers its last-used layout first.

When no saved layouts exist, a directory opens directly into a shell.
Save a layout first to try the multi-pane workflow.
To add periodic saves and a tab line with repository status, install the sibling
[project-autosave](../project-autosave/README.md) and [tab-line](../tab-line/README.md) packages.

## Reload, uninstall and recovery

An edit to `config.json`, to a module or to the manifest reloads the package as a unit.
After an edit that is not valid, the last working version stays active.
`afterglow ctl scripts` shows `last-good` and the error.
Correct the edit to recover.
When a new version replaces the old version, Afterglow cancels the open dialogs and provider jobs of the old version.

To uninstall the package, remove the `scripts/project-navigator/` directory.
Afterglow removes its commands, its open selector and its jobs automatically.
Favorites and history stay in `state/scripts/project-navigator.json` beside the configuration file.
A new installation restores them.

To reset the favorites and the history:

1. Remove the `scripts/project-navigator/` directory.
2. Remove the file `state/scripts/project-navigator.json`.
3. Copy the package into `scripts/` again.

## Tests

Run `python3 afterglow/tests/navigator_smoke.py build/afterglow/afterglow` from the repository for an isolated live check.
It needs `dbus-run-session`, Bash and the dependencies of the nested desktop below.
It also needs the `aglow` that the build puts next to `afterglow`, so keep `AFTERGLOW_BUILD_AGLOW` on.
It runs in a nested KWin with a private D-Bus session and a temporary configuration and state.
Then it closes its windows.

The native acceptance check needs KWin, an X11 or XWayland display, libX11, xprop, ImageMagick,
`dbus-run-session`, Bash, Git and fd:

```bash
python3 afterglow/tests/navigator_ui_smoke.py build/afterglow/afterglow
```

The test creates a nested desktop with private D-Bus, configuration and state.
It drives the picker with keyboard input.
It checks window reuse, configuration reload, removal with an open selector, reinstallation and retained layouts after a restart.
It also checks periodic save and restore, and the tab line toggle, in the companion packages.
Screenshots and logs go to a new temporary directory whose name starts with `afterglow-navigator-ui-`.
The test prints its path. Change it with `--artifacts`.
The test closes only its temporary windows and processes.
