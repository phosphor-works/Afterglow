# Scene controls

Install with:

```sh
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/scene-controls ~/.config/afterglow/scripts/
```

Open **Edit scene parameters and presets** in the command palette. Choose an
active shader occurrence, then select a preset, edit a parameter (including
quality/intensity), save its current values, or reset this package's changes.
All selectors/prompts are native, keyboard operated and system themed. Escape
cancels. Updates target the current tab; window effects target the whole window.
Changing shaders while editing expires the selection safely: reopen the editor.

Presets use Phosphor's registry and file schema under
`~/.config/afterglow/shader-presets/surface/`. The family directory is an internal
reuse of Phosphor's surface preset namespace, isolated from PlasmaZones presets.
Pack presets are read-only; Save creates a new UUID-named preset. Saved presets
survive restart, but active script overrides are transient. Reapply a saved
preset after restart. Preset application starts from pack defaults; Reset
releases this package's overrides to reveal other scripts/configuration.

Values are validated, and edits animate for 350 ms. Remove this package to clear
its overrides and pending dialogs. Saved presets remain; delete their JSON files
to remove them from the picker. At most 128 user presets are stored. The editor
shows supported numeric, boolean and color parameters; image parameters remain
outside the live API. A preset with unsupported/unknown parameters is rejected.
