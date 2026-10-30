<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Afterglow

A GPU-accelerated terminal for Wayland, built on Qt and Vulkan. Every pane of
every window draws through one GPU device.

Start with the [user reference](docs/README.md), [complete configuration keys](docs/CONFIGURATION.md),
[desktop integration](docs/DESKTOP.md) and [compatibility examples](docs/COMPATIBILITY.md).
See [release notes](CHANGELOG.md) and [migration guidance](docs/MIGRATION.md) when upgrading.

## Requirements

- Qt 6.8 or later (Core, Gui, Widgets, Qml, Quick, QuickControls2, Concurrent, DBus, ShaderTools, and the GuiPrivate,
  WaylandClientPrivate and QuickPrivate headers)
- A Vulkan driver
- FreeType, HarfBuzz, FriBidi, fontconfig, xkbcommon, wayland-client, zlib, PCRE2 (32-bit library)
- KF6 WindowSystem
- The phosphor libraries from a PlasmaZones checkout (see below)
- Optional: `rsvg-convert` (renders the icon sizes), `tic` (compiles the
  terminfo entry), GTest (tests), `cava` (system-audio-reactive shaders)

## Building

The phosphor libraries are built from source alongside afterglow. By default
the build expects a PlasmaZones checkout next to this one:

```sh
git clone https://github.com/phosphor-works/afterglow
cd afterglow
git clone https://github.com/fuddlesworth/PlasmaZones ../PlasmaZones
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Point `-DAFTERGLOW_PHOSPHOR_LIBS_DIR=/path/to/PlasmaZones/phosphor-libs` at
another checkout if needed.

## Installing

```sh
sudo cmake --install build --prefix /usr
```

This installs `bin/afterglow`, the desktop entry, AppStream metadata, icons,
shader packs and the `afterglow` terminfo entry. Afterglow's libraries and its
own copies of the phosphor libraries go to a private `lib/afterglow/`, so a
system install of phosphor (from PlasmaZones) is left alone.

## Configuration

`~/.config/afterglow/config.json`, reloaded live. Shader packs come from the
installed `share/afterglow/shaders` and `~/.config/afterglow/shaders`.

On the command line, `-o Group.key=value` overrides a setting for one launch
(`afterglow -o Font.size=14 -o Colors.theme=afterglow-light`), `--config`
reads another file and `--class` sets the Wayland app id for window rules.
Those three change the whole process, so such a launch runs on its own
instead of opening its window in the afterglow already running. `--title`
fixes the window title and `--hold` keeps the window open after an `-e`
program exits.

Ordinary launches reuse the running process only if it uses the same
executable file. A development build or a freshly replaced binary starts
separately, so an older running copy cannot silently handle its new windows.

### Font rendering

Tabs, menus, prompts, and search use the desktop's Qt control style, palette,
and UI font. On Plasma this uses the installed `qqc2-desktop-style` and
platform theme integration; `QT_QUICK_CONTROLS_STYLE` can select another
installed style. Terminal colors and fonts remain configurable separately.

The live-reloaded `Font` group accepts:

| Setting | Values | Default |
|---|---|---|
| `antialiasing` | `grayscale`, `none` (monochrome), `rgb`, `bgr` | `grayscale` |
| `hinting` | `none`, `light` (vertical snapping), `full` | `light` |
| `gamma` | 0.5–3.0; higher values strengthen antialiased edges | 1.0 |

For example, `afterglow -o Font.antialiasing=rgb -o Font.gamma=1.2`.
RGB/BGR describe horizontal display subpixel order; choose the order matching
your panel. These modes blend filtered FreeType coverage independently for
each color channel. Windows with active shader chains or translucency, and
panes with a translucent background, automatically use separately rasterized
grayscale text. Returning to an opaque, shader-free window restores subpixel
text. Compositor scaling or rotating the output can make grayscale preferable.

Gamma adjusts glyph coverage (`coverage^(1/gamma)`), not the display's gamma
or the scene's color space. At 1.0 the existing grayscale appearance is
preserved. Monochrome font edges remain binary; color emoji retain their
original pixels. Procedural box drawing and powerline shapes keep their own
antialiasing, with coverage gamma also applied to their masks. Font rendering
settings apply to terminal cells; Qt Quick draws the window chrome separately.

### System audio

Shaders opt into system-playback analysis with `#define AFTERGLOW_AUDIO 1`.
Afterglow uses PhosphorAudio's CAVA provider, automatically choosing PipeWire
or PulseAudio. Install `cava` to enable it. One analyzer serves all windows;
it runs only while an exposed, non-minimized window has an audio shader.
No audio is recorded to disk. Missing CAVA or silence leaves the scene's
normal animation running.

The live-reloaded `Audio` group accepts `enabled` (default `true`),
`inputMethod` (`auto`, `pipewire`, or `pulse`) and `inputSource` (default `auto`,
the backend's default playback source). Set `Audio.enabled` to `false` to
stop capture, or use `afterglow -o Audio.enabled=false` for one launch.
Explicit source names are backend-specific; for PulseAudio, use an output's
monitor source. Afterglow does not change the default output or its volume.

Opted-in shaders receive:

| Input | Meaning |
|---|---|
| `getBass()`, `getMids()`, `getTreble()`, `getOverall()` | Smoothed normalized RMS energy, 0–1; also available as `iAudioLevels.xyzw`. |
| `audioBar(int index)` | One of 64 mono logarithmic bands, low to high (50 Hz–10 kHz); out-of-range indices return zero. |
| `audioBarSmooth(float position)` | Interpolated spectrum at a normalized position, 0–1. Independent of pane coordinates. |
| `iAudioSpectrumSize` | 64 while receiving valid frames, otherwise 0. |
| `uAudioSpectrum` | 64×1 R32F texture at binding 3; use the helpers or `texelFetch` to bypass the terminal's `texture()` coordinate mapping. |

Bass covers bins 0–19, mids 20–47, and treble 48–63. These are visual energy
levels with automatic gain, not calibrated sound-pressure measurements.
Stopped, disabled, failed or stale capture yields zero; ordinary shaders
also receive zero. Audio is independent of the eight Luau `iSignal` channels.
Static audio shaders redraw on spectrum changes without a continuous timer.

### Scrollback-driven shaders

`iScrollback` lets a shader respond to retained terminal history:

| Component | Meaning |
|---|---|
| `.x` | This pane's retained primary-screen rows; for a window pass, the focused pane's. |
| `.y` | Combined retained rows across panes in the **current tab**. |
| `.z` | The same combined count eased with a 1.25-second time constant. |
| `.w` | Reserved, zero. |

Background tabs and other windows do not contribute. Switching tabs, closing
panes, trimming the buffer or clearing history changes the total; dropped rows
and visible screen rows do not count. Full-screen programs retain the influence
of the shell's primary-screen history. Counts measure rows, so terminal reflow
can change them. Reading this input copies counts, without scanning history text.
Static shaders referencing `iScrollback` receive redraws while the eased count
settles, then return to rendering only on changes.

### Themes

`Colors.theme` names a theme; its colours replace the ones in `Colors`.
Themes come from `~/.config/afterglow/themes`, then the installed
`share/afterglow/themes` (`afterglow-dark` and `afterglow-light`). Theme files
can be afterglow JSON (the `Colors` keys), Ghostty themes or kitty themes, so
collections such as iTerm2-Color-Schemes drop straight in. To follow the
desktop's light or dark preference, write `"light:afterglow-light,dark:nord"`.
A path works too, so a theme another tool generates stays in step.

### Keybinds

`Keybinds.bindings` maps an action to a key or a list of keys; an empty
string unbinds it. Besides the built-in actions, `"text:<bytes>"` sends bytes
to the terminal (`\e \r \n \t \\ \xHH \uHHHH`), for example
`"text:\\x1b\\r": "Shift+Return"`, and `"script:<id>.<action>"` runs a script
action.

Sequences use comma-separated keys (up to four), and `Leader` expands to
`Keybinds.leader`. Named tables provide temporary modes:

```json
{
  "Keybinds": {
    "leader": "Ctrl+Space",
    "sequenceTimeout": 1000,
    "tableTimeout": 5000,
    "bindings": {
      "split_right": "Leader, E",
      "table:resize": "Leader, R"
    },
    "tables": {
      "resize": {
        "resize_left": "H",
        "resize_right": "L",
        "resize_up": "K",
        "resize_down": "J",
        "pop_key_table": "Return"
      }
    }
  }
}
```

The active sequence/table appears in the window. Escape, timeout, focus changes,
and config reload cancel it. A mismatched key exits the mode and is processed
normally; consumed prefixes are never replayed to the shell. Tables remain active
after matching actions until cancelled, and may activate another table. Ambiguous
sequences and prefixes overlapping existing single-key shortcuts are rejected;
unbind the original action explicitly to reuse its key as a prefix. Tables and
sequences support built-in, `text:` and `script:` actions.

Pane controls are also available through the command palette, `ctl action`,
and Luau actions:

| Action | Default shortcut | Behavior |
| --- | --- | --- |
| `toggle_pane_zoom` | Ctrl+Shift+Return | Fill the tab with the focused pane; toggle again to restore the split layout |
| `resize_left`, `resize_right` | Ctrl+Shift+Alt+Left/Right | Move a nearby vertical divider by two character cells |
| `resize_up`, `resize_down` | Ctrl+Shift+Alt+Up/Down | Move a nearby horizontal divider by two character cells |

Zoom preserves split ratios and pane identities. Directional focus switches
which pane is zoomed; splitting or resizing restores the layout first. Hidden
panes keep running at their previous terminal size. Resizing clamps nested
splits to a minimum of four columns and two rows plus padding. At an outside
edge, a resize moves the nearest divider on that axis in the requested direction.

### Moving panes and tabs

`swap_left/right/up/down` exchange the focused pane with its neighbour.
`move_pane_to_tab` moves it to a new tab; `move_pane_to_window` and `detach_tab`
create a window for one pane or the entire split tab. Bind these actions or
use the command palette. Processes, history, graphics and pane IDs survive;
a detached whole tab also retains its ID and split ratios.

`afterglow ctl move-pane 34 right --pane 12` moves pane 12 beside pane 34,
including across tabs/windows. Use target `0` with `new-tab` or `new-window`.
`afterglow ctl detach-tab --pane 12` detaches its whole tab. Luau has matching
`afterglow.move_pane(pane, target, placement)` and `afterglow.detach_tab(pane)`
effects. Request results contain the pane snapshot and destination IDs;
`on_pane_moved(ev)` receives the updated location without a fake
creation/closure event. Moving the last pane closes the emptied window.

### Named projects

Use **Save Project** in the command palette to save the current window's tabs,
ordered split trees, ratios, active tab/panes, user titles, working directories
and explicit launch arguments. **Switch Project** searches saved names and
focuses an already-open project; otherwise it launches the saved layout in a
new window. These actions are `save_project` and `switch_project` for keybinds.
The original windows remain open. Replacing an existing save requires confirmation.

```sh
afterglow ctl save-project development --pane 12
afterglow ctl projects
afterglow ctl open-project development          # focus existing, or launch
afterglow ctl open-project development --new    # launch another instance
afterglow ctl save-project development --pane 12 --overwrite
```

Layouts live in `projects/` beside the active config file (normally
`~/.config/afterglow/projects/`). Names use 1–64 letters, digits, underscores or
hyphens. Saves use atomic replacement and reject symlink files. The version-1
JSON format allows up to 16 tabs, 32 panes, tree depth 16 and 1 MiB per file.
Invalid layouts are rejected before creating windows or running commands.

Copy [development.json](data/examples/projects/development.json) into that
folder, adjust its directories and commands, then open `development`. A leaf
has `pane: {id, cwd, title, command}`; a split has `direction` (`right` or `down`),
`ratio` (strictly between 0 and 1), `first` and `second`. Each tab has `id`,
`title`, `active_pane` and `tree`; the document has `version`, `active_tab` and
`tabs`. Saved pane IDs must be unique across the document; tab IDs must also
be unique. They are labels mapped to fresh runtime IDs on every launch.

`command` is an argv array: `[]` starts the configured shell. Only commands
explicitly supplied when a pane was created are captured; foreground commands
and shell history are never inferred. There is no implicit shell expansion;
use an explicit shell with `-c` when wanted. Directories must be absolute or
start with `~/` (`~` alone means home). Missing directories fall back to home;
unavailable commands fall back to a shell with a warning. Explicit commands
stay visible after exit, including their exit status; restored shells also
stay visible if they exit unsuccessfully. Press a key to close a held pane.
These are launch layouts: scrollback, environment snapshots, remote sessions,
and running process state are not persisted. Review commands in imported layouts
before opening them, just as for a shell script.

Luau exposes `save_project(pane, name, overwrite?)`,
`open_project(name, newInstance?)` and `list_projects()` effects. Wrap them with
`afterglow.request(id, effect)` to receive results: saving returns `name` and
`window_id`; listing returns `names`; opening returns `name`, `window_id` and
`reused`. A fresh launch also returns `pane_ids` and `tab_ids` maps (saved labels
to runtime IDs) and `warnings`. Opening projects shares the structural action
budget. Window snapshots include `project` (empty for unnamed windows).
[projects.luau](data/examples/scripts/projects.luau) demonstrates a native
selector driven by these APIs. D-Bus offers `Projects`, `SaveProject` and
`OpenProject`; the CLI prints save/open results as JSON.

For an opt-in integration check on a running Wayland desktop, run
`python3 afterglow/tests/project_smoke.py build/afterglow/afterglow`.
It creates its own D-Bus session and temporary config, checks live pane transfers,
save/restore and failure recovery, then closes its test windows.

### History, search, and hints

`copy_mode` (Ctrl+Shift+Space) enters keyboard selection. Move with arrows or
hjkl, PageUp/PageDown, Home/End, or Ctrl+Home/End for history boundaries.
Press v for character selection, Shift+V for lines, Ctrl+V for a rectangle,
and y or Return to copy and leave. Escape/q cancels; / opens search. Wide
characters and combining sequences remain whole. A resize invalidates the
selection rather than copying stale coordinates. The focused pane has a
themed scrollbar showing its position within scrollback.

Search's `.*` toggle enables PCRE2 regular expressions. Search joins soft
wraps and maps matches back to cells, including wide and combining characters.
Lowercase queries are case insensitive; uppercase makes them case sensitive.
Capture is incremental and matching runs in a worker. Results are capped at
10,000 matches over the first 4 Mi characters; a notice reports truncation or
regex complexity/time limits. Search is a snapshot; reopen or edit the query
to refresh after new output. Resize during capture cancels that search.

`show_last_output` opens the last marked command output in a new tab using
`Terminal.pager` (default `less -R`). Output is limited to 1 MiB with a
truncation notice. The pager receives a temporary file, removed when its pane
closes; the command uses argv parsing without shell expansion. Missing shell
marks or an unavailable pager produce a notification.

`show_hints` (Ctrl+Shift+H) labels regex matches in the current viewport.
The default matches file:line references and copies the selected match.
Replace `Hints.patterns` to customize it:

```json
{
  "Hints": {
    "patterns": [
      {"pattern": "[A-Za-z0-9_./-]+:[0-9]+", "action": "copy"},
      {"pattern": "[A-Za-z0-9_./-]+:[0-9]+:[0-9]+", "action": "command",
       "command": ["kate", "--line", "{line}", "--column", "{column}", "{path}"]}
    ]
  }
}
```

Actions are `copy`, `open` (a local file through the desktop), or `command`
(explicit argv, no shell). Placeholders are `{match}`, `{path}`, `{line}`,
and `{column}`; paths resolve against the pane's working directory.
Matching is bounded to 16 patterns, 64 Ki characters and 1,000 distinct matches.
Typing a label accepts it; Escape cancels. Changing pane content invalidates
these hints. Existing URL hints remain available separately.

### Shell integration

New panes automatically integrate interactive Bash (4.4+), Zsh and Fish (4+).
Prompt and command boundaries, command exit status, and working-directory updates
feed command navigation and Luau events. Bash and Zsh retain their user startup
files and prompt hooks; Fish retains its native `mark-prompt` behavior. No
user dotfiles are modified. Login and interactive flags are supported; shell
scripts, `-c` commands and explicit startup overrides such as `--norc`, `-f`
or `--no-config` are left alone.

Set `Terminal.shellIntegration` to `false`, or launch with
`AFTERGLOW_SHELL_INTEGRATION=0`, to disable automatic injection. This applies to
new panes and does not remove manually configured hooks or Fish's native marks.
Custom shell wrappers and older Fish versions need their own prompt integration.

## Shaders

Shaders are Ghostty-style GLSL (`mainImage`, `iChannel0`, the cursor uniforms)
or phosphor shader packs, listed in five chains under `Shaders`:

- `background`, `text` and `cursor` each run per pane on that layer alone,
  before the layers are put together. Backgrounds include images under the
  text; text includes decorations and images over it. A layer's input is
  premultiplied and transparent where the layer has nothing, and the shader
  must return premultiplied colour too.
- `pane` runs per pane on the finished pane.
- `window` runs once over the whole window.

The tab bar and other chrome are drawn after every chain. With no layer
shaders, the layers are never split apart and cost nothing.

Shaders can also react to what happens in the terminal: `iSignal[0..7]`
hold a value and the time it was set, per pane, set by scripts
(`afterglow.signal`), `afterglow ctl signal` or D-Bus. `data/examples/glasshouse`
uses all of this to dress afterglow for the Glasshouse desktop.
[Phosphor](data/examples/phosphor/README.md) combines a shared 3D containment core,
text afterimages and scripted command reactions in an isolated demo session.

Shaders can sample `iPreviousText` for the previous frame's processed text
layer, using the same region-relative coordinates as `iChannel0`. It starts
transparent and resets on layout, theme and shader changes. Referencing it
enables layer rendering, retains a text-history texture and schedules
animation frames; shaders that do not use it incur no history allocation or copy.

For bloom and other effects that need highlight headroom, put
`#define AFTERGLOW_HDR 1` in the shader source. If any shader requests it, that
window's intermediate targets and text history use RGBA16F instead of RGBA8.
This retains values above 1 between passes; it does not change the display
output or automatically convert colour spaces. The shader chain must tone-map
its radiance before the final output. Phosphor does this in its background
lens pass, before compositing text. HDR doubles intermediate texture memory;
ordinary shader chains retain their existing format.

Packs can declare up to eight persistent buffers with Phosphor's metadata,
per-buffer clamp/repeat/mirror wrapping and linear/nearest filtering. The
terminal adapter provides live image parameters, auxiliary depth attachments,
and a custom vertex hook alongside explicit sRGB/linear conversion helpers.
See [Feedback glow and the buffer contract](data/shaders/feedback-glow/README.md)
for ordering, reset rules and memory limits, and [Depth trails](data/shaders/depth-trails/README.md)
for a runnable two-buffer, depth, vertex and image-parameter example.

## Control and scripting

Other programs drive afterglow over D-Bus (`io.github.fuddlesworth.Afterglow.Control`
at `/Afterglow/Control`), or with `afterglow ctl`: list panes, send text, split,
open tabs and windows, set titles, notify, run actions, and `watch` events as
JSON lines. Run `afterglow ctl --help` for the full list. Reading a pane's text
needs `"Control": { "allowReadText": true }`; `"Control": { "enabled": false }`
turns the interface off.

Luau scripts in `~/.config/afterglow/scripts/` react to events (commands
finishing, bells, titles, focus) and add actions to the command palette, which
bind in `Keybinds` as `"script:<id>.<action>"`. Scripts run sandboxed, with
no direct file or process API and a short time limit. Installed scripts are trusted:
file reads and process launches are available through explicit effects. They return effects, which
afterglow rate limits and carries out. Types for editors are installed to
`share/afterglow/api/afterglow.d.luau`, and examples to
`share/afterglow/examples/scripts`. `"Scripts": { "enabled": false }` turns
scripting off.

The [project navigator package](data/examples/packages/project-navigator/README.md)
combines saved layouts, running projects, Git discovery, optional zoxide,
favorites and persistent history in a native fuzzy picker.
The [project autosave package](data/examples/packages/project-autosave/README.md)
adds opt-in periodic layout saves and a restore picker; the
[tab/status package](data/examples/packages/tab-status/README.md) adds configurable
Git, directory, command and exit-status components. These are native Luau examples
inspired by sessionizer.wezterm, resurrect.wezterm and tabline.wez, respectively.
Each package includes installation, configuration, walkthrough and removal instructions.

### Luau packages and persistent state

Single `.luau` scripts remain supported. A package is a directory under the
active configuration's `scripts/` directory, containing `package.json`:

```json
{
  "id": "my-extension",
  "version": "1.0.0",
  "api_version": 1,
  "state_version": 1,
  "entry": "main",
  "modules": {"main": "init.luau", "format": "format.luau"},
  "config": {"enabled": true},
  "dependencies": {"utils": {"path": "vendor/utils", "version": "1.0.0"}},
  "assets": {"theme": "assets/theme.json"}
}
```

The ID must match the package directory. `version` is an `X.Y.Z` release;
`api_version` must be `1`. Each module returns a table. `require("format")`
loads a declared module once per instance; `require("@utils")` loads the entry
of a vendored package with its own manifest and the exact declared version.
Imports are local: nothing is downloaded or installed automatically. Dependencies
share the caller's sandbox and state namespace. Cycles and undeclared imports
fail with diagnostics. Up to 16 packages and 64 modules may be bundled, with
256 KiB per source file and 1 MiB total source/manifest/config data.

Optional `config.json` beside the manifest overrides its `config` keys.
Package `on_load(ev)` receives `ev.package` (ID, version, API version, home
path, config and declared asset paths) and `ev.storage` (value, schema version,
revision). Asset declarations can accompany themes, shader preset files and
images: up to 32 local files, 4 MiB each and 16 MiB total. They are exposed as
paths; declaration does not activate a theme or shader. Paths and symlinks
cannot escape the package root. Source files, dependency manifests and config
are watched together; edits replace the complete instance. Compilation,
initialization or migration failures preserve the previous working instance.

```lua
local script = {}
function script.on_load(ev)
    local state = ev.storage.value
    state.launches = (state.launches or 0) + 1
    return afterglow.request("saved", afterglow.state_write(state, ev.storage.revision))
end
return script
```

`afterglow.state_read()` and `state_write(value, revision)` are correlated effects;
wrap them in `afterglow.request` to receive `{value, version, revision}` through
`on_result`. Storage is scoped to the script ID under `state/scripts/` beside the
active config. Writes use a lock, revision comparison and atomic replacement.
A conflict returns `state_error`; reread and merge before retrying. Values must
be JSON-compatible object tables (nested lists supported), within 256 KiB,
16 levels and 8192 values. Nonfinite numbers, unsupported values, symlinks and
corrupt files are rejected. Corrupt data is retained for manual recovery.
Single-file scripts use schema version 1 and can use the same read/write effects.

Increase the manifest's `state_version` when the stored shape changes, and
export `migrate(value, oldVersion, newVersion)` returning the migrated object.
Migrations and `on_load` run under the 100 ms action deadline. Migration output
is validated and committed only after the replacement compiles and initializes;
exceptions, timeouts and revision conflicts retain the original file and live
instance. Downgrades to an older state schema are refused. `on_load` should
return effects; effects take place after successful activation, and their
operation failures are reported normally through `on_result`.

`afterglow ctl scripts` reports active, disabled, failed and last-good instances,
including paths and load errors. Fix a file to retry loading. In-memory module
state survives unchanged reloads, but resets on replacement, removal or process
exit. Only explicit disk state survives those boundaries. Package removal
cleans up its tasks and UI contributions, while its stored data remains.

`afterglow.open_directory(path, layout?)` opens a directory with a shell or a
saved project layout, placing every template pane in that directory. A window
already associated with the canonical directory is focused without restarting
it. Results contain `window_id`, `pane_id`, `cwd` and `reused`, plus mappings and
warnings for a layout restore. Window snapshots expose `project_directory`.
The CLI equivalent is `afterglow ctl open-directory PATH --layout NAME`.

### Terminal state in Luau

Every handler receives `ev.terminal`, a version 1 snapshot with `windows`,
`tabs`, `panes`, `active_window`, `active_tab` and `active_pane`. It contains
metadata, not terminal text. It is a detached copy: editing it cannot change
the terminal. Snapshots are captured before dispatch; queued events can describe
an earlier command while the snapshot already reflects more recent activity.
The existing `on_status` field `ev.state` remains the program's status string.

Window records contain `id`, `focused`, `active_tab` and `active_pane`. Tab
records contain `id`, `window`, `index`, `title`, `active` and `active_pane`;
`active` means selected in its window. Pane records include `id`, `window`,
`tab_id`, `tab`, `title`, `cwd`, `command`, `cols`, `rows`, `focused` and
`context`. Each window retains its selected tab and pane when unfocused.
The top-level active IDs are zero when no Afterglow window has keyboard focus.

Pane, tab and window IDs last for the process lifetime. `pane.tab_id` is stable
across tab reordering; the existing `pane.tab` and D-Bus tab field remain
zero-based positions. IDs do not persist across restart. Closed-pane events
retain `id`, `tab_id` and `window`; the closed pane is absent from the live snapshot.

`afterglow.panes(ev.terminal, scope, anchor)` filters a snapshot. Scope is
`"all"` (the default), `"pane"`, `"tab"` or `"window"`. The anchor is a pane
record or ID; omitted, it uses the globally active objects. A missing anchor
ID produces no matches, rather than targeting another pane.

```lua
function script.run_action(action, ev)
    local panes = afterglow.panes(ev.terminal, "tab", ev.pane)
    return afterglow.log("This tab has " .. #panes .. " panes")
end
```

### Script lifecycle, tasks, and native UI

Scripts may define `on_load`, `on_unload`, and `on_config_changed`.
Unchanged source retains its instance; a broken edit keeps the last working
instance. Successful replacement, removal, disable, or repeated handler failure
cancels its tasks and clears its UI contributions. Unload runs with the normal
handler time limit; returned effects are ignored because resources are being
retired. Packages can use declared modules and revisioned durable state; see the
package lifecycle below.

All task names are local to a script instance (1–64 characters), with 16 active
tasks per script and 128 globally. Reusing an active name reports `busy`.
`afterglow.cancel(name)` cancels that owned timer, watcher, or job.

| Effect | Behavior |
| --- | --- |
| `timer(name, delay_ms, repeat_ms?)` | Calls `on_timer(ev)` with `ev.name`; omit repeat for one shot. Repeat is at least 250 ms and starts after the handler returns, so callbacks cannot overlap. |
| `exec(name, argv, options?)` | Runs an explicit argv list asynchronously and sends `on_result` using name as request ID. Options: `cwd`, `timeout_ms` (default 30,000; max 600,000), `max_bytes` (default 65,536; max 1 MiB). |
| `read_file(name, path, options?)` | Asynchronously reads a regular file with the same timeout/byte options; returns `text` and `truncated`. Eight file workers maximum; special files are rejected. |
| `watch_file(name, path)` | Watches an existing regular file, including atomic replacement, and calls `on_file_changed` with `name`, `path`, `exists`; changes debounce for 250 ms. |

Process results include `stdout`, `stderr`, `exit_code`, and `truncated`
on normal completion. Errors include `spawn_failed`, `process_failed`,
`timeout`, `output_limit`, `cancelled`, and `io_error`. Exceeding the combined
stdout/stderr cap terminates the job and returns partial output. Timeout,
cancellation, and unload terminate its process group. Callbacks for retired
instances are discarded. Timer/watch delivery is capped at 64 callbacks per
second per instance.

Wrap `select(pane, title, items)` or `prompt(pane, title, initial_text?)`
in `afterglow.request`. Selectors accept up to 500 unique `{id, title}`
items with fuzzy filtering; results return the selected `id/title`.
Prompts return `text`. Escape, pane closure, or window closure cancels;
a second dialog in that window reports `busy`. These use the desktop's Qt
Quick Controls theme.

`set_actions({{id=..., title=...}, ...})` replaces up to 32 palette actions
for that script, which must supply `run_action`.
`status(pane, text?, tab_title?)` replaces its status label and tab suffix;
empty strings clear them. `override(pane, values)` replaces its pane-local
appearance overrides: `cursorBlink` (boolean), `backgroundOpacity`, and
`unfocusedDim` (0–1). For each field the latest applied script override wins,
above the loaded configuration; removing it reveals the next owner's value
or configuration. Empty values clear that owner's overrides. All contributions
are removed automatically on unload and pane closure.

`read_text(pane, scope?, max_bytes?)` returns bounded terminal text through
`afterglow.request`: scopes are `screen`, `scrollback`, and `last-output`
(default), with a default 64 KiB and maximum 1 MiB. Results contain `text`,
`truncated`, and `available`; missing command marks set `available=false`.
Installed scripts can read terminal text independently of the external D-Bus
read-text switch. The shipped `build-inspector.luau` example extracts compiler
diagnostics from the last output, offers a fuzzy selector, and opens the selected
file and line in a new editor tab.

### Operation results

Wrap any effect in `afterglow.request("request-id", effect)` to receive its
result in that script's `on_result(ev)`. Request IDs are nonempty strings of
at most 64 characters; use distinct IDs when requests need separate tracking.
Existing effects without a request ID keep their current behavior.

```lua
function script.run_action(action, ev)
    if ev.pane then
        return afterglow.request("workspace", afterglow.split(ev.pane, "right"))
    end
end

function script.on_result(ev)
    if ev.request_id ~= "workspace" then return nil end
    if not ev.ok then return afterglow.log(ev.error.message) end
    return afterglow.set_title(ev.data.pane, "Workspace")
end
```

Results contain `request_id`, `kind`, `ok`, `data`, a fresh `terminal`
snapshot, and the initiating event's `pane` when available. Failures include
`error = { code, message }`: `invalid_effect`, `no_pane`, `rate_limited`,
`operation_failed` or `unavailable`. Direct `split`, `new_tab` and `new_window`
effects return `data.pane`, `pane_id`, `tab_id` and `window_id`. Other effects
return an empty data table unless documented otherwise.

Results are queued after application, only to the originating script instance.
The result records what happened at application time; its fresh snapshot may
already show the created pane closed. Follow-up effects validate the ID again.
Success means the host accepted/applied the operation, not that a spawned
process finished or a generic action's asynchronous UI prompt was confirmed.
Use direct creation helpers when a returned pane identity is required.

Unloading, replacing or disabling a script cancels its outstanding results.
An unchanged script retains its VM and pending results during reload. Handlers
may return at most 64 effects; larger lists apply nothing and log an error.
Malformed request IDs do not receive results. A script exceeding 64 queued
or 64 results per second is disabled until its source changes, cancelling
pending callbacks; refusals count too, preventing an endless retry loop.

The existing limits remain: 16 MiB per VM, 20 ms per event, 100 ms per user
action, 20 applied effects per second, and three structural effects per ten
seconds. Generic actions that create or close panes, tabs or windows use the
same structural budget as direct effects. Effect lists apply in order and are
not transactions. See [create-and-title.luau](data/examples/scripts/create-and-title.luau)
for a complete action with a result handler.

### Scoped shader signal batches

`afterglow.signals(pane, values, scope)` updates 1–8 distinct channels in one
effect. Scope is `"pane"`, `"tab"` (default), or `"window"`, relative to the
anchor pane at application time. Each value is `{ channel = 0, value = 1 }`;
channels are integers 0–7 and values must fit in a finite float.

```lua
return afterglow.signals(ev.pane, {
    { channel = 2, value = 0 },
    { channel = 4, value = 0 },
    { channel = 0, value = 1 },
}, "tab")
```

The host validates the entire batch and its target set before changing any
signal. Up to 128 panes receive all channels in one GUI-thread turn, with the
same timestamp; painting sees the complete update. A batch consumes one effect
from the rate budget. When requested, its result lists updated pane IDs in
`data.panes`. The original `afterglow.signal` helper remains available.

The Phosphor example uses current snapshots and tab batches, so loading it
after panes already exist works without rebuilding a pane registry. Command
activity in background tabs does not charge the visible tab's scene.

### Live named shader parameters

`afterglow.shaders(pane)` returns the window's active effect instances through
`afterglow.request` / `on_result`, in `data.shaders`. Each has `instance`,
`shader`, `layer`, `parameters` (IDs, types, defaults, ranges, units and a
`supported` flag), and the effective `values` for that pane. Repeated uses of
one pack have distinct instance IDs. Raw shaders without metadata are listed
with no editable parameters. Image parameters accept absolute local file paths;
an empty string clears the texture. Pack defaults resolve relative to the pack directory. Image updates
are validated and decoded before applying the entire parameter update.

`afterglow.shader_parameters(pane, instance, values, scope?)` replaces this
script's overrides for the selected instance and target panes. Scope defaults
to `"pane"`; `"tab"` and `"window"` select the anchor's current tab or window
at application time. These are snapshots of the target set, not subscriptions
for newly created panes. A window-layer effect requires `"window"` scope and
has one window-owned value independent of pane focus. Other layers can be
updated across at most 128 panes in one atomic operation and one effect budget
unit. Results include the affected `data.panes`.

```lua
return afterglow.request("inspect", afterglow.shaders(ev.pane))
```

In `on_result`, select an instance from `ev.data.shaders`, then return:

```lua
return afterglow.shader_parameters(ev.pane, instance.instance,
    { strength = 0.8 }, "tab")
```

Numbers must be finite, integer parameters require integral values, booleans
require booleans, and colors require valid Qt color strings such as `"#ff0077"`.
Unknown names, unsupported types/slots, and out-of-range values reject the whole
update. Phosphor supplies metadata and range validation; script values are not
silently clamped or replaced. Configuration loading also uses Phosphor's
validation/default handling.

An empty values table resets this script's overrides on the selected targets.
The most recent successful write wins per parameter across scripts; reset or
unload reveals any other script's value, then the configured/default value.
Writes replace the owner's previous table, so omitted parameters are released.
Shader/configuration rebuilds clear overrides and expire instance IDs; query
again after a stale-ID error. Moving a pane to another window drops its source
window's parameter overrides. Closing panes releases their overrides. Values
are transient uniforms and image bindings: they neither edit configuration nor recompile
shader programs. Pass an optional fifth argument `{duration_ms=650, curve="ease-in-out"}` to
animate toward the new values with Phosphor's host animation runtime. Supported
curves are `linear`, `ease-in-out` and a critically damped `spring`; duration is
0–60000 ms (0 applies immediately), and is a hard lifetime ceiling for springs.
Retargeting preserves the currently visible position and starts a new duration.
Float parameters interpolate within their endpoints; colors interpolate in
linear-light sRGB through Phosphor and return encoded color uniforms. Integer,
boolean and image parameters switch at completion. Omitted parameters and empty-table
resets release overrides immediately. Unload, rebuild, pane closure and transfer
cancel owned transitions. Completion results acknowledge acceptance, not the
end of the animation. Hidden windows catch up when rendered again.

Install [shader-parameters.luau](data/examples/scripts/shader-parameters.luau)
in `~/.config/afterglow/scripts/` for palette actions that boost or reset the
first active Bloom instance in the current tab (or window for a window effect).
Enable Bloom in a shader chain first; the example reports if none is active.

### Scene presets and telemetry

Install [scene-controls](data/examples/packages/scene-controls/README.md) for
native effect/preset selectors, quality/intensity editing, save and reset.
The host exposes `shader_presets(pane, instance)`,
`shader_preset(pane, instance, preset, scope?, transition?)`, and
`save_shader_preset(pane, instance, name)`, all with normal operation results.
Discovery returns `data.presets`; save returns `data.id` and `data.name`.
`@defaults` applies pack defaults, while an empty `shader_parameters` table
releases the calling script's changes. Saved presets use Phosphor's schema and
registry under `~/.config/afterglow/shader-presets/surface/`, separate from
PlasmaZones. Pack presets are read-only; saves create new named entries and
survive restarts. Applying presets is transient until explicitly reapplied.

`scene_state(pane)` returns `data.scene`: the anchor window's **current** tab ID,
combined `history_rows`, `history_normalized = rows / (rows + 5000)`, monotonic
`sample_ms`, `audio_available`, and 0–1 `bass`, `mids`, `treble`, `overall` levels.
Samples are cached for 100 ms; changing tabs invalidates that cache. History
always counts the current tab, even when the anchor pane is in a background tab.
Audio observes the existing shared analyzer; querying does not start capture.
Unavailable/stale capture gives false availability and zero levels. Scripts
can sample with bounded timers; continuous FFT inputs and interpolation stay
in the host. [contextual-scene](data/examples/packages/contextual-scene/README.md)
shows a once-per-second decision driving a smooth Phosphor exposure transition.

## Licence

The application is GPL-3.0-or-later; the libraries under `libs/` are
LGPL-2.1-or-later; the icon is CC-BY-SA-4.0. Each file names its licence;
the texts are in `LICENSES/`.
