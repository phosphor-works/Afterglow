<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Project autosave

This package periodically saves enabled projects and provides a native restore picker.
It uses ideas from [resurrect.wezterm](https://github.com/MLFlexer/resurrect.wezterm).
It does not run WezTerm plugins.

Install from the repository root:

```sh
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/project-autosave ~/.config/afterglow/scripts/
```

When you use a custom configuration file, use the `scripts/` directory beside that file.
The package loads immediately. Installation does not enable autosave.

## Use autosave

1. Arrange the tabs and splits.
2. Select **Save Project** in the command palette.
3. Give the layout a name.
4. Select **Enable autosave for this saved project**.

This immediately saves the window and enables periodic saves for that name.
**Autosave replaces the named layout with current tabs, split ratios, directories and explicit launch commands.**
Use a separate name to preserve a template.

Changes are saved every 60 seconds by default.
Select **Save project now** for an immediate save before exit.

After restart, select **Restore an autosaved project**.
The picker lists enabled projects with saved layouts.
An existing window with the same project name receives focus instead of a duplicate window.

Select **Disable autosave for this saved project** to stop periodic saves for its name.
The saved layout remains in the normal project switcher.

## Configuration and limits

Create `config.json` beside `package.json` to change the interval:

```json
{"interval_seconds": 60}
```

The interval is clamped to 10–3600 seconds.
Each interval saves up to eight windows sequentially. Larger sets rotate across intervals.
An open restore picker does not stop the periodic saves.
Duplicate live windows with the same project name are skipped to prevent conflicting saves.
The package remembers enablement for up to 100 names.
Failed saves are reported. The next interval can retry.
Autosave does not run commands.

Restore runs commands stored in the layout.
It does not preserve running processes, editor buffers, terminal scrollback or shell history.
There is no guaranteed save on shutdown.
Recovery uses the latest completed periodic or manual save.
Separate Afterglow processes do not synchronize live layouts.
Use one application instance for each autosaved project.

## Commands and removal

| Command | Palette title | Action |
| --- | --- | --- |
| `save [NAME]` | **Save project now** | Saves the window now, under its project name or under `NAME`. |
| `enable` | **Enable autosave for this saved project** | Turns autosave on for the window's project and saves it once. |
| `disable` | **Disable autosave for this saved project** | Turns autosave off for the window's project. |
| `restore` | **Restore an autosaved project** | Opens the restore picker. |

Bind a command as `script:project-autosave.save`, `script:project-autosave.enable` and so on.
From a shell, run a command as `aglow run project-autosave COMMAND`.
The command acts on the window of the shell's pane.
Only `save` takes an argument.

`save NAME` gives the window the project name `NAME`. Autosave then uses the setting of `NAME`.
Run `enable` to autosave `NAME`.
`save` replaces only the window's own saved project.
When `NAME` is the name of another saved project, `save` fails and changes nothing.
Use **Save Project** in the command palette to replace another project.
`disable` for a project that has autosave off changes nothing.

| Exit status | Cause |
| --- | --- |
| 0 | The command completed. `save`, `enable` and `disable` write a message on stdout. `restore` writes nothing. |
| 1 | The command failed. The message is on stderr. |
| 2 | An argument that the command does not take. |

A command from the palette or a key shows its message as a notification on the pane.

Remove `scripts/project-autosave/` to uninstall.
Afterglow then stops the autosave timer and closes an open restore picker.
Saved layouts remain in `projects/` beside the configuration.
Enablement state remains in `state/scripts/project-autosave.json` for reinstallation.
