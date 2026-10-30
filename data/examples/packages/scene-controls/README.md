<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Scene controls

Install the package:

```sh
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/scene-controls ~/.config/afterglow/scripts/
```

1. Select **Edit scene parameters and presets** in the command palette.
2. Select an active shader instance.
3. Select a preset or edit a parameter.

The editor also saves current values and resets this package's changes.
Selectors and prompts use native controls, keyboard input and the system theme.
Escape cancels. Changes target the current tab, or the whole window for window effects.
Shader changes expire the selection. Reopen the editor after a shader change.

Presets use Phosphor's registry and schema in `~/.config/afterglow/shader-presets/surface/`.
The surface namespace is separate from PlasmaZones presets.
Pack presets are read-only. Save creates a new UUID-named preset.
Saved presets survive restart. Active script overrides do not.
Apply a saved preset again after restart.
Preset application starts from pack defaults.
Reset releases this package's overrides and exposes other script or configuration values.

The host validates all values. Edits animate for 350 ms.
The editor supports numeric, boolean, color and image parameters.
Images change at transition completion.
See [image limits](../../../../docs/SHADER-API.md#images).
Presets with unknown or unsupported parameters are rejected.

Remove the package directory to clear its overrides and pending dialogs.
Saved presets remain. Delete their JSON files to remove them from the picker.
The store permits 128 user presets.
