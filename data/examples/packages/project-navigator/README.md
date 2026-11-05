<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Project navigator

This Luau package provides project discovery and selection.
It uses ideas from [sessionizer.wezterm](https://github.com/mikkasendke/sessionizer.wezterm).
It uses native selectors, asynchronous jobs, project layouts and persistent state.

Copy this complete directory into `~/.config/afterglow/scripts/project-navigator/`.
When you use a custom configuration file, use the `scripts/` directory beside that file.
With the `/usr` installation prefix, examples are installed in `/usr/share/afterglow/examples/packages/project-navigator/`.
Loading the package does not run commands.

From the repository root, install it with:

```bash
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/project-navigator ~/.config/afterglow/scripts/
```

The destination must contain `scripts/project-navigator/package.json`, not
`scripts/package.json`. Afterglow automatically reloads scripts when they change.

Open the command palette and choose:

- **Navigate projects**: search running projects, saved layouts, favorites,
  recent projects and discovered directories. Running projects are reused.
  For a new directory, choose a saved layout or a shell. The last-used layout
  is offered first next time. Every template pane starts in that directory.
- **Toggle favorite project directory**: pin or unpin the current project directory
  (or the pane's working directory for an ordinary window).
- **Previous project**: return to the most recent different project.

These are the commands `pick`, `favorite` and `previous`. Bind them as
`script:project-navigator.pick`, `script:project-navigator.favorite` and
`script:project-navigator.previous`.

From a shell:

```sh
afterglow run project-navigator open ~/Projects/site dev   # open a directory, with the layout dev
afterglow run project-navigator list                       # favorite, recent and saved projects
find ~/src -maxdepth 1 -mindepth 1 -type d | afterglow run project-navigator import
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
Thus one directory has one entry, also when the shell reached it through a link.
Without `realpath`, a directory keeps the path that the shell gave.

`list` leaves out a value that contains a tab or a line break, and says so on stderr.
`import` skips a line that is not a path or that names a path over 256 bytes.

| Exit status | Cause |
| --- | --- |
| 0 | The command completed. |
| 1 | The command failed, or `import` skipped lines. The message is on stderr. |
| 2 | Wrong arguments, or a `DIR` that cannot be a directory. |

A command from the palette or a key has no shell.
Its failure shows as a notification on the pane.

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
| `roots` | `["~/Projects"]` | Directories to offer and to search for Git repositories. A root starts with `/`, `~` or `~/`. The package reads the first eight entries. |
| `fd` | `true` | `false` turns off the search for Git repositories with `fd`. |
| `zoxide` | `true` | `false` turns off the directories from `zoxide`. |
| `max_depth` | `5` | How many directory levels `fd` searches below each root, from 1 to 16. A value outside this range changes to the nearest limit. |

`fd` discovers Git repositories, including worktrees with `.git` files.
`zoxide query --list` supplies frequently used directories.
Both commands are optional. The picker title names each provider that did not start, timed out or was refused, and why.
For example, `(unavailable: fd-1 timed out)` or `(unavailable: zoxide not started)`.
Configured roots, saved layouts, favorites and history remain available.
Providers run concurrently.
A provider that exits with an error still supplies the complete paths that it printed.
A provider that is refused supplies no paths.

| Item | Limit | At the limit |
| --- | --- | --- |
| Roots | 8 | The package ignores the other entries of `roots`. |
| Provider run time | 5 seconds | The provider stops and supplies no paths. |
| Provider output | 128 KiB | The provider stops. It supplies the complete paths that it printed. |
| Choices in the picker | 400 | The picker does not show more choices. Favorites, recent projects, running projects and roots come first. |
| Favorites | 100 | A new favorite fails. `import` skips the other lines and exits with status 1. |
| Lines in one `import` | 61 | `import` reads the first 61 lines that are not empty. It does not read the other lines, and exits with status 1. |

Finish or cancel the current operation before starting another.
Escape cancels a selector without starting a project.
When the run of a command ends while it waits, for example at Ctrl+C on `afterglow run`, no selector opens after it.

Favorites, the last 50 projects and layout choices survive reloads and restarts.
The package keeps layout choices for favorites and recent directories only.
Favorites and layout choices accept paths of up to 256 bytes.
Storage is scoped to `project-navigator`, under
`state/scripts/project-navigator.json` beside the active configuration. Schema 2
includes a migration example from schema 1's arrays of favorite and recent paths.
State conflicts preserve the newer file and report the failed save. The next
command reads current state again. A launch can succeed even if saving its history
fails. `afterglow ctl scripts` reports package load errors and retained last-good
instances.

`providers.luau` handles discovery and parsing. `history.luau` handles bounded
state and migrations. `init.luau` coordinates commands and correlated results.
The package uses only declared local imports and Afterglow effects.

Run `python3 afterglow/tests/navigator_smoke.py build/afterglow/afterglow`
from the repository for an isolated live check. It needs `dbus-run-session`,
Bash and the dependencies of the nested desktop below. It runs in a nested KWin
with a private D-Bus session and a temporary configuration and state, then closes its windows.

## Walkthrough: reuse a split layout across repositories

1. Open two or three terminal splits.
2. Choose **Save Project** in the command palette.
3. Name the layout `development`.
4. Choose **Navigate projects**.
5. Type part of a repository path.
   With `fd` installed, Git repositories beneath the configured roots appear
   automatically. Roots themselves remain usable without discovery tools.
6. Press Enter.
7. Choose `development` in the **Layout for …** picker. A new window opens with
   the saved split structure, and every pane starts in the chosen repository.
   Explicit commands saved with the layout run again. Commands typed into an
   interactive shell are not recorded as launch commands.
8. Choose **Toggle favorite project directory** in the new window.
9. Open the navigator again.
   That directory is marked with a star and `[running]`.
10. Select another repository.
11. Choose a layout for it.
12. Choose **Previous project**.
    Afterglow returns to the earlier directory. Selecting a running directory
    reuses its window and running processes.
13. Restart Afterglow.
    Favorites and recent directories remain in the picker.
    Selecting a directory offers its last-used layout first.

A directory opens directly into a shell when no saved layouts exist. Save a
layout first to exercise the multi-pane workflow. To add periodic saves and a
tab line with repository status, install the sibling [project-autosave](../project-autosave/README.md)
and [tab-line](../tab-line/README.md) packages.

## Reload, uninstall and recovery

Edits to `config.json`, modules and the manifest reload the package as a unit.
An invalid edit leaves the last working version active. `afterglow ctl scripts`
shows `last-good` and the error. Correct the edit to recover. Open dialogs and
provider jobs from a successfully replaced version are canceled.

Remove the `scripts/project-navigator/` directory to uninstall. Its commands,
open selector and jobs are removed automatically. Favorites and history stay in
`state/scripts/project-navigator.json` beside the configuration file, so a new
installation restores them.

To reset the favorites and the history:

1. Remove the `scripts/project-navigator/` directory.
2. Remove the file `state/scripts/project-navigator.json`.
3. Copy the package into `scripts/` again.

The native acceptance check requires KWin, an X11 or XWayland display, libX11,
xprop, ImageMagick, `dbus-run-session`, Bash, Git and fd:

```bash
python3 afterglow/tests/navigator_ui_smoke.py build/afterglow/afterglow
```

The test creates a nested desktop with private D-Bus, configuration and state.
It drives the picker with keyboard input.
It checks window reuse, configuration reload, removal with an open selector, reinstallation and retained layouts after restart.
It also checks periodic save and restore, and the tab line toggle, in the companion packages.
Screenshots and logs go to a new temporary directory whose name starts with `afterglow-navigator-ui-`.
The test prints its path. Change this with `--artifacts`.
The test closes only its temporary windows and processes.
