<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Scene controls

Install the package:

```sh
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/scene-controls ~/.config/afterglow/scripts/
```

## Edit a scene

1. Select **Edit scene parameters and presets** in the command palette.
2. Select an active shader instance.
3. Select a preset or edit a parameter.

This is the command `open`. Bind it as `script:scene-controls.open` in `Keybinds.bindings`.
The editor also saves current values and resets this package's changes.
Selectors and prompts use native controls, keyboard input and the system theme.
Escape cancels. Changes target the current tab, or the whole window for window effects.
Shader changes expire the selection. Reopen the editor after a shader change.

Presets use Phosphor's registry and schema in `~/.config/afterglow/shader-presets/surface/`.
The surface namespace is separate from PlasmaZones presets.
Pack presets are read-only. Save makes a new preset, with a UUID as its file name.
Saved presets survive restart. Active script overrides do not.
Apply a saved preset again after restart.
Preset application starts from pack defaults.
An edit keeps only the values that this package sets. It does not copy values that other scripts set.
Reset removes the overrides of this package. The values of other scripts or of the configuration then apply.

The host validates all values. Edits animate for 350 ms.
The editor supports numeric, boolean, color and image parameters.
Images change at transition completion.
See [image limits](../../../../docs/SHADER-API.md#images).
Presets with unknown or unsupported parameters are rejected.

## Commands from a shell

The commands `get` and `set` read and set the parameters of the pane's shaders:

```sh
aglow run scene-controls get              # INSTANCE<TAB>PARAMETER<TAB>VALUE lines
aglow run scene-controls get strength     # one parameter
aglow run scene-controls set strength 1.4
```

`set` changes the first shader of the pane that has the parameter, in the same layer as the editor.

| Parameter type | `VALUE` |
| --- | --- |
| Number | A decimal number, such as `1.4`, `-2` or `5e-1` |
| Whole number | A decimal number with no fraction, such as `3` |
| Boolean | `true` or `false` |
| Other | The text of the value |

| Exit status | Cause |
| --- | --- |
| 0 | The command completed. |
| 1 | The pane has no such parameter, or Afterglow refused the value. The message is on stderr. |
| 2 | Wrong arguments, or a `VALUE` that does not fit the parameter type. |

`set` and `open` fail while the editor or another `set` is open. Thus only one change runs at a time.

## Remove the package

Remove the package directory to clear its overrides and pending dialogs.
Saved presets remain. Delete their JSON files to remove them from the picker.
The store permits 128 user presets.
