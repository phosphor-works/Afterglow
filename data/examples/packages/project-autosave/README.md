# Project autosave

An Afterglow package inspired by the save/restore workflow of
[resurrect.wezterm](https://github.com/StephenGemin/resurrect.wezterm).
It periodically saves explicitly enabled projects and offers a native restore picker.
This is an Afterglow implementation, not a compatible WezTerm plugin port.

From the repository root:

```bash
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/project-autosave ~/.config/afterglow/scripts/
```

For a custom config, use its adjacent `scripts/` directory. The package loads
immediately. Installation alone does not enable autosaving any project.

1. Arrange tabs and splits, then choose **Save Project** in the command palette
   and give the layout a name.
2. Choose **Enable autosave for this saved project**. This immediately saves the
   window and opts that name into periodic saves. **Autosave overwrites that
   named layout with its current tabs, split ratios, directories and explicit
   launch commands.** Use a separate name if you want to preserve a template.
3. Change the layout. It is saved every 60 seconds by default; **Save project
   now** saves immediately before quitting.
4. After restarting Afterglow, choose **Restore an autosaved project**. The
   picker lists enabled projects that still have a saved layout. An existing
   window with the same project name is focused instead of duplicated.
5. **Disable autosave for this saved project** stops periodic saves for its name.
   The saved layout remains available through the normal project switcher.

Create `config.json` beside `package.json` to adjust the interval:

```json
{"interval_seconds": 60}
```

The interval is clamped to 10–3600 seconds. Each interval saves at most eight
windows sequentially, rotating through larger sets. Duplicate live windows with
the same project name are skipped to avoid overwriting one another. Enablement
is persisted for up to 100 project names. A failed save is reported; the next
interval can retry. No commands run during autosave itself.

Restore launches the commands stored in the layout. It does not serialize running
processes, editor buffers, terminal scrollback or shell history. There is no
shutdown save guarantee: the latest completed periodic or manual save is the
recovery point. Other running Afterglow processes do not synchronize live layouts;
use one application instance for a given autosaved project.

Actions bind as `script:project-autosave.enable`, `.disable`, `.save`, `.restore`.
Remove `scripts/project-autosave/` to uninstall; timers and pending UI are cleaned
up. Saved layouts in `projects/` and opt-ins in
`state/scripts/project-autosave.json` beside the config remain for reinstall.
