<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Afterglow

A GPU-accelerated terminal for Wayland, built on Qt and Vulkan. Every pane of
every window draws through one GPU device.

## Requirements

- Qt 6.8 or later (Core, Gui, Widgets, Qml, Quick, QuickControls2, DBus, ShaderTools, and the GuiPrivate,
  WaylandClientPrivate and QuickPrivate headers)
- A Vulkan driver
- FreeType, HarfBuzz, fontconfig, xkbcommon, wayland-client, zlib
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
no direct file or process API and a short time limit. They return effects, which
afterglow rate limits and carries out. Types for editors are installed to
`share/afterglow/api/afterglow.d.luau`, and examples to
`share/afterglow/examples/scripts`. `"Scripts": { "enabled": false }` turns
scripting off.

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
zero-based positions. IDs do not persist across restart. A closed pane can
appear as only `{ id = ... }` in a queued event and is absent from the snapshot.

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

## Licence

The application is GPL-3.0-or-later; the libraries under `libs/` are
LGPL-2.1-or-later; the icon is CC-BY-SA-4.0. Each file names its licence;
the texts are in `LICENSES/`.
