<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Panes and projects

## Move panes and tabs

| Action | Function |
| --- | --- |
| `swap_left`, `swap_right`, `swap_up`, `swap_down` | Exchanges the focused pane with an adjacent pane |
| `rotate_panes_clockwise` (Ctrl+Shift+]), `rotate_panes_counterclockwise` (Ctrl+Shift+[) | Moves every pane in the tab to the next or previous position in reading order |
| `pane_overview` (Ctrl+Shift+D) | Shows the panes of every tab as live cards to choose from |
| `move_pane_to_tab` | Moves the pane to a new tab |
| `move_pane_to_window` | Moves the pane to a new window |
| `detach_tab` | Moves the complete tab to a new window |

Rotation keeps the layout shape and split ratios. Focus stays with the same pane.

### Pane overview

The pane overview shows the panes of every tab in the window as live cards, up to 32.
Each tab's cards form a group, in tab order. The focused pane starts highlighted.

| Key | Function |
| --- | --- |
| Right, Tab or `l` | Highlights the next pane |
| Left, Shift+Tab or `h` | Highlights the previous pane |
| Down or `j`, Up or `k` | Highlights the first pane of the next or previous tab |
| Home, End | Highlights the first or last pane |
| Enter, Space or a left click | Focuses the highlighted pane and closes the overview |
| 1 to 9 | Focuses that numbered card and closes the overview |
| Escape, the `pane_overview` key, or a right or middle click | Closes the overview and keeps the focus |

The mouse wheel also moves the highlight.
A pane in another tab switches to that tab.
Keys do not reach the panes while the overview is open. Programs in the panes keep running.
A zoomed tab shows all of its panes in the overview, and returns zoomed on the chosen pane.
`Animation.overviewCurve` and `Animation.overviewDuration` set the motion.
`Shaders.overview` chooses the pack that draws it. See [shaders](SHADERS.md#shader-chains).
Use the command palette or a keybinding to run these actions.
Drag a tab outside the tab bar and release it to open a new window.
Alternatively, right-click the tab and select **Detach Tab to New Window**.
A horizontal drag inside the bar changes tab order.
Movement keeps processes, history, graphics and pane IDs.
A detached tab also keeps its tab ID and split ratios.
An empty source window closes after its last pane moves.

These commands move pane 12 beside pane 34, then detach its tab:

```sh
afterglow ctl move-pane 34 right --pane 12
afterglow ctl detach-tab --pane 12
```

A move can cross tab and window boundaries.
Use target `0` with `new-tab` or `new-window`.
Luau supplies matching `move_pane` and `detach_tab` effects.
Their results contain the pane snapshot and destination IDs.
`on_pane_moved` reports the new location. A move does not cause creation or closure events.

## Save and open projects

1. Open the command palette.
2. Select **Save Project**.
3. Enter a project name.

A project stores one window's tabs, split trees, ratios, focus, titles, directories and explicit launch arguments.
Replacing an existing project requires confirmation.
**Switch Project** searches saved names.
It focuses an open instance or starts the saved layout in a new window.
Other windows remain open.
The keybinding action names are `save_project` and `switch_project`.

```sh
afterglow ctl save-project development --pane 12
afterglow ctl projects
afterglow ctl open-project development          # focus existing, or launch
afterglow ctl open-project development --new    # launch another instance
afterglow ctl save-project development --pane 12 --overwrite
```

## Project files

Layouts are in `projects/` beside the active configuration file.
The usual path is `~/.config/afterglow/projects/`.
Names contain 1–64 letters, digits, underscores or hyphens.
Saves replace files atomically. Symlink files are not permitted.

| Limit | Value |
| --- | --- |
| Format version | 1 |
| Tabs | 16 |
| Panes | 32 |
| Tree depth | 16 |
| File size | 1 MiB |

Afterglow validates the layout before it creates windows or runs commands.

1. Copy [development.json](../data/examples/projects/development.json) into the project directory.
2. Set the directories and commands for your system.
3. Open the `development` project.

| Object | Fields |
| --- | --- |
| Document | `version`, `active_tab`, `tabs` |
| Tab | `id`, `title`, `active_pane`, `tree` |
| Leaf | `pane: {id, cwd, title, command}` |
| Split | `direction`, `ratio`, `first`, `second` |

Split direction is `right` or `down`.
The ratio must be greater than zero and less than one.
Pane IDs must be unique in the document. Tab IDs must also be unique.
Saved IDs are labels. Each launch maps them to new runtime IDs.

## Commands and recovery

`command` is an argument array. An empty array starts the configured shell.
Only explicit pane launch commands are saved.
Afterglow does not infer commands from foreground processes or shell history.
Commands do not use implicit shell expansion.
Use an explicit shell with `-c` when expansion is necessary.

Directories must be absolute or start with `~/`. A single `~` means the home directory.
A missing directory uses the home directory instead.
An unavailable command starts a shell with a warning.
Explicit commands remain visible after exit, with their exit status.
A restored shell also remains visible after an unsuccessful exit.
Press a key to close a held pane.

Projects store launch layouts, not running sessions.
They do not save scrollback, environment snapshots, remote sessions or process state.
Examine commands in imported layouts before you open them.

## Script and D-Bus results

| Luau effect | Result fields |
| --- | --- |
| `save_project(pane, name, overwrite?)` | `name`, `window_id` |
| `list_projects()` | `names` |
| `open_project(name, newInstance?)` | `name`, `window_id`, `reused` |

Wait for the result with `afterglow.await`.
A new project instance also returns `pane_ids`, `tab_ids` and `warnings`.
The ID maps connect saved labels to runtime IDs.
Project creation uses the structural action budget.
Window snapshots contain `project`. An unnamed window has an empty project name.

The [project script](../data/examples/scripts/projects.luau) shows a native selector.
D-Bus supplies `Projects`, `SaveProject` and `OpenProject`.
The CLI prints save and open results as JSON.

`open_directory(path, layout?)` starts a shell or saved layout in a directory.
All template panes use that directory.
An existing window for the canonical directory receives focus without a restart.
Results contain `window_id`, `pane_id`, `cwd` and `reused`.
A layout restore also returns ID maps and warnings.
Window snapshots contain `project_directory`.

```sh
afterglow ctl open-directory PATH --layout NAME
```

## Integration test

Run this command on a Wayland desktop:

```sh
python3 afterglow/tests/project_smoke.py build/afterglow/afterglow
```

The test uses a private D-Bus session and temporary configuration.
It checks pane transfers, save, restore and error recovery.
It closes its test windows when it finishes.
