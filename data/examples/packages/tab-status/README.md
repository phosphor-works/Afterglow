# Tab and repository status

An Afterglow package inspired by the configurable components in
[tabline.wez](https://github.com/michaelbrusegard/tabline.wez). It displays the
working directory, Git branch/dirty marker, running command, last exit result
and remote context using Afterglow's native tab/status contributions.

From the repository root:

```bash
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/tab-status ~/.config/afterglow/scripts/
```

For a custom config, use its adjacent `scripts/` directory. Within a few seconds,
the active pane of each tab supplies its title and status. Run a command, change
directory or switch panes to see updates. **Toggle tab and repository status** in
the palette enables/disables it; the choice survives restart. Keybind token:
`script:tab-status.toggle`.

Create `config.json` beside `package.json` to change the component order:

```json
{
  "segments": ["cwd", "git", "command", "exit", "context"],
  "separator": "  |  ",
  "tab_title": true,
  "git": true,
  "git_refresh_seconds": 15
}
```

`cwd` shows the directory basename. `git` shows branch and `*` for tracked changes;
untracked files are excluded. `command` shows the running shell-integrated command
or `idle`. `exit` shows `ok` or `exit N` after a command finishes. `context` shows
shell-provided remote/root identity. Command and exit components need shell
integration. Unknown segment names are ignored; at most eight are displayed.

One tab is refreshed per second, rotating across all tabs. Git runs asynchronously,
with no shell interpolation, no optional index locks, a two-second timeout and
16 KiB output limit. Results are cached for 15 seconds by default (5–300 allowed).
Command completion invalidates that directory's cache. Missing Git, non-repositories
and failed/bounded queries omit the Git segment. Remote-context panes never query
local Git for their reported directory. There are no network Git operations.

Text uses the system theme through existing Qt chrome. This package does not add
per-segment colors, icons, powerline shapes or custom tab widgets; those need a
richer UI contribution API. Avoid installing multiple packages that replace the
same tab title/status simultaneously.

Remove `scripts/tab-status/` to uninstall. Its timer, Git job and UI contributions
are removed; `state/scripts/tab-status.json` beside the config retains the toggle.
