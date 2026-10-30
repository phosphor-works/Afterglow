# Project navigator

An Afterglow Luau package inspired by the configurable discovery menus in
[sessionizer.wezterm](https://github.com/mikkasendke/sessionizer.wezterm).
It uses Afterglow's native selector, async jobs, project layouts and durable state.

Copy this entire directory into `~/.config/afterglow/scripts/project-navigator/`
(or `scripts/` beside your custom config). Packaged installations also ship it
under `/usr/share/afterglow/examples/packages/project-navigator/` when using the
usual `/usr` prefix. No commands run merely by loading the package.

From the repository root, install it with:

```bash
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/project-navigator ~/.config/afterglow/scripts/
```

The destination must contain `scripts/project-navigator/package.json`, not
`scripts/package.json`. Afterglow automatically reloads scripts when they change.

Open the command palette and choose:

- **Navigate projects** — search running projects, saved layouts, favorites,
  recent projects and discovered directories. Running projects are reused.
  For a new directory, choose a saved layout or a shell; the last-used layout
  is offered first next time. Every template pane starts in that directory.
- **Toggle favorite project directory** — pin/unpin the current project directory
  (or the pane's working directory for an ordinary window).
- **Previous project** — return to the most recent different project.

Keybind tokens are `script:project-navigator.open`,
`script:project-navigator.favorite` and `script:project-navigator.previous`.

Create `config.json` beside `package.json` to override the defaults:

```json
{
  "roots": ["~/Projects", "~/work"],
  "fd": true,
  "zoxide": true,
  "max_depth": 5
}
```

`fd` discovers Git repositories (including `.git` files for worktrees), and
`zoxide query --list` contributes frequently used directories. Both are
optional; missing commands are reported in the picker title. Configured roots,
saved layouts, favorites and history remain available. Providers run concurrently
with a five-second timeout and bounded output. Up to eight roots and 400 choices
are used. An already-open operation must finish or be cancelled before starting
another one. Escape cancels a selector without launching anything.

Favorites, the last 50 projects and layout choices survive reloads/restarts.
Storage is scoped to `project-navigator`, under
`state/scripts/project-navigator.json` beside the active configuration. Schema 2
includes a migration example from schema 1's arrays of favorite/recent paths.
State conflicts preserve the newer file and report the failed save; the next
action reads current state again. A launch can succeed even if saving its history
fails. `afterglow ctl scripts` reports package load errors and retained last-good
instances.

`providers.luau` handles discovery and parsing; `history.luau` handles bounded
state and migrations; `init.luau` coordinates actions and correlated results.
The package uses only declared local imports and Afterglow effects.

Run `python3 afterglow/tests/navigator_smoke.py build/afterglow/afterglow`
from the repository on a Wayland desktop for an isolated live check. It uses
a private D-Bus session and temporary config/state, then closes its windows.
