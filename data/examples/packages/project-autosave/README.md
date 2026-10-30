<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Project autosave

This package periodically saves enabled projects and provides a native restore picker.
It uses ideas from [resurrect.wezterm](https://github.com/StephenGemin/resurrect.wezterm).
It does not run WezTerm plugins.

Install from the repository root:

```sh
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/project-autosave ~/.config/afterglow/scripts/
```

For a custom configuration, use the adjacent `scripts/` directory.
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

## Actions and removal

Action names are `script:project-autosave.enable`, `.disable`, `.save` and `.restore`.
Remove `scripts/project-autosave/` to uninstall.
The host removes timers and pending interfaces.
Saved layouts remain in `projects/` beside the configuration.
Enablement state remains in `state/scripts/project-autosave.json` for reinstallation.
