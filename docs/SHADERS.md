<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Shaders

Afterglow shaders are Phosphor shader packs.
A pack is a directory with `metadata.json` and GLSL stages.
Each stage uses the terminal shader contract and defines `vec4 pTerminal(vec2 uv)`.
Configuration refers to packs by ID.
The [shader API](SHADER-API.md) describes the contract.

## Shader chains

The `Shaders` configuration group contains five chains and the pane overview:

| Chain | Input |
| --- | --- |
| `background` | Each pane's background, including images below text. |
| `text` | Each pane's text, decorations and images above text. |
| `cursor` | Each pane's cursor layer. |
| `pane` | The completed pane. |
| `window` | The completed window. |
| `overview` | The completed window, while the pane overview shows. One pack ID, not a list. |
| `tabTransition` | The completed window, while a tab switch runs. One pack ID, not a list. |
| `paneEffects` | The completed window, while a pane opens or closes. One pack ID, not a list. |

The first three chains run before pane composition.
Layer inputs use premultiplied alpha. Empty areas are transparent.
Shader output must also use premultiplied alpha.
Afterglow draws the tab bar and other controls after all shader chains.
Without layer shaders, Afterglow does not allocate separate layers.

The pane overview (`pane_overview`, Ctrl+Shift+D) shows the panes of every tab in 3D.
The default overview pack is Pane Deck: live cards on a carousel, reflected in a floor.
Set `Shaders.overview` to another pack ID, or to an empty string to turn the overview off.
Script packages can ship packs of their own, such as an overview with the script that drives it. See [packages](PACKAGES.md#shaders).
The [shader API](SHADER-API.md#panes-and-the-overview) describes how to write an overview pack.

Switching tabs runs `Shaders.tabTransition`. The default pack is Tab Switch.
Its `style` parameter chooses a turning cube (0), a page flip (1), a shatter (2) or a slide (3).
The tab being left keeps running until the new tab arrives.
`Animation.tabCurve` and `Animation.tabDuration` set the motion.
Set `Shaders.tabTransition` to an empty string to switch tabs at once.

Splitting and closing panes run `Shaders.paneEffects`. The default pack is Pane Effects.
Its `style` parameter chooses a hinge (0) or a picture tube (1).
A new pane swings in from the edge it split from, or switches on like a tube.
A closed pane falls back off its bottom edge, or switches off to a dot.
`Animation.paneEffectDuration` sets the time. `Animation.splits=false` also turns the effects off.
A pane whose shell moves to another host, user or container passes through a portal, such as after `afterglow ssh`.
While it is away, a soft frame marks it: cyan for a remote host, red for an elevated user and magenta for a container.
The `portal` and `frame` parameters set their strength.

[Phosphor](../data/examples/phosphor/README.md) combines a 3D core, text afterimages and command reactions.
[Glasshouse](../data/examples/glasshouse/README.md) provides another complete desktop scene.

## Previous text and color

Include `terminal_history.glsl` to read `uPreviousText`, the previous frame's processed text layer.
Sample it with the canvas's `uv`, as with `terminalTexel`.
It starts transparent. Layout, theme and shader changes reset it.
The include enables layer rendering, a history texture and animation frames.
Other shaders require no text-history allocation or copy.

Set `"highDynamicRange": true` in the pack metadata for values above 1 between passes.
One requesting shader changes the window's intermediate targets and text history from RGBA8 to RGBA16F.
This doubles intermediate texture memory.
It does not change display output or convert color spaces.
The chain must tone-map its radiance before final output.
Phosphor does this in its background lens pass, before text composition.

Packs support eight buffers, four image parameters, auxiliary depth output and a vertex stage.
Mesh passes add imported or shader-generated triangles, instancing and hardware depth testing.
[Mesh Reactor](../data/shaders/mesh-reactor/README.md) demonstrates this path.
See the [shader API](SHADER-API.md) for pass order, coordinates, reset conditions and memory limits.
[Depth trails](../data/shaders/depth-trails/README.md) demonstrates these interfaces.

## System audio

1. Install `cava`.
2. Add `#include <terminal_audio.glsl>` to the shader.

PhosphorAudio uses CAVA to analyze system playback through PipeWire or PulseAudio.
One analyzer serves all windows.
It runs only when an exposed window has an audio shader and is not minimized.
Afterglow does not record audio to disk or change output selection or volume.
Silence or missing CAVA does not stop normal scene animation.

The `Audio` configuration group reloads while Afterglow runs:

| Key | Values and behavior |
| --- | --- |
| `enabled` | Default: `true`. Set `false` to stop capture. |
| `inputMethod` | `auto`, `pipewire` or `pulse`. |
| `inputSource` | Default: `auto`, the backend's default playback source. |

Explicit source names depend on the backend.
For PulseAudio, use the output's monitor source.
Use `afterglow -o Audio.enabled=false` to disable capture for one launch.

| Input | Meaning |
| --- | --- |
| `getBass()`, `getMids()`, `getTreble()`, `getOverall()` | Smoothed RMS energy from 0 to 1. Also available as `uAudioLevels.xyzw`. |
| `audioBar(int index)` | One of 64 mono logarithmic bands from 50 Hz to 10 kHz. Invalid indices return zero. |
| `audioBarSmooth(float position)` | Interpolated spectrum at a position from 0 to 1. Pane coordinates have no effect. |
| `iAudioSpectrumSize` | 64 for valid frames; otherwise zero. |
| `uAudioSpectrum` | A 64×1 R32F texture at binding 10. Use the helpers or `texelFetch`. |

Bass uses bins 0–19. Mids use bins 20–47. Treble uses bins 48–63.
Automatic gain adjusts these visual levels. They are not calibrated sound-pressure measurements.
Stopped, disabled, failed or stale capture produces zero values.
Shaders without the audio include also receive zero.
Audio inputs are independent of the eight Luau `uSignal` channels.
Static audio shaders redraw when the spectrum changes. They do not need a continuous timer.

## Scrollback

`uScrollback` provides retained terminal history counts:

| Component | Meaning |
| --- | --- |
| `.x` | This pane's retained primary-screen rows. A window pass uses the focused pane. |
| `.y` | Combined retained rows in the current tab. |
| `.z` | The combined count, smoothed with a 1.25-second time constant. |
| `.w` | Reserved. Always zero. |

Background tabs, other windows, visible screen rows and discarded rows do not contribute.
Tab changes, pane closure, buffer trimming and history clearing change the total.
Full-screen programs retain the influence of primary-screen history.
Reflow can change the count because the unit is rows.
Afterglow reads cached counts without scanning history text.
Static shaders redraw until the smoothed value settles. They then redraw only for changes.

## Script controls

The [Luau API](SCRIPTING.md) returns effects for host execution.
Use `afterglow.await` when you need a result.

### Signals

`uSignal[0..7]` holds a value for each pane and channel, and the seconds since it was set.
Read them with `signalValue(channel)` and `signalAge(channel)`.
A channel that was never set has an age of 1,000,000 seconds.
Set a channel with `afterglow.signal`, `afterglow ctl signal` or D-Bus.

`afterglow.signals(pane, values, scope)` changes 1–8 distinct channels in one effect.
Channels must be integers from 0 to 7. Values must fit in a finite float.
Scope is `pane`, `tab` or `window`. The default is `tab`.
The host resolves the scope from the anchor pane when it applies the effect.

```lua
return afterglow.signals(ev.pane, {
    { channel = 2, value = 0 },
    { channel = 4, value = 0 },
    { channel = 0, value = 1 },
}, "tab")
```

The host validates all values and targets before any change.
Up to 128 panes receive the batch in one GUI-thread turn, with one timestamp.
Rendering sees the complete update. The batch uses one effect from the rate budget.
A requested result lists the changed pane IDs in `data.panes`.
The Phosphor example uses snapshots and tab batches, including when loaded after pane creation.
Background-tab commands do not increase the visible scene's activity.

### Parameters

`afterglow.shaders(pane)` returns active window effect instances in `data.shaders`.
Each entry contains `instance`, `shader`, `layer`, `parameters` and effective `values` for the pane.
Parameter metadata contains IDs, types, defaults, ranges, units and a `supported` flag.
Repeated pack instances have different instance IDs.

```lua
local active = afterglow.await(afterglow.shaders(ev.pane))
for _, instance in active and active.shaders or {} do
    if instance.shader == "bloom" then
        return afterglow.shader_parameters(ev.pane, instance.instance, { strength = 0.8 }, "tab")
    end
end
```

`shader_parameters(pane, instance, values, scope?)` replaces the calling script's overrides for the selected targets.
The default scope is `pane`. Other scopes are `tab` and `window`.
The host resolves targets when it applies the effect. New panes do not inherit the update.
Window-layer effects require `window` scope. Their value belongs to the window and does not depend on pane focus.
Other layers permit up to 128 target panes.
Each update is atomic and uses one effect. Results include `data.panes`.

Numbers must be finite. Integer parameters require integers. Boolean parameters require booleans.
Colors require valid Qt color strings, such as `#ff0077`.
Unknown names, unsupported types or slots, and values outside the range reject the complete update.
Phosphor validates metadata and ranges. Script values are not clamped.
Configuration loading uses Phosphor validation and defaults.
Images use the [image path and size rules](SHADER-API.md#images).
The host validates and decodes images before it applies any part of the update.

An empty values table releases the script's overrides on the selected targets.
The latest successful write wins for each parameter across scripts.
Reset or unload exposes another script's value, then the configured value or default.
An update replaces the owner's previous table. Omitted parameters are released.
Shader or configuration rebuilds clear overrides and expire instance IDs.
Query again after a stale-ID error.
Moving a pane between windows removes its source-window overrides. Closing a pane releases its overrides.
Updates change temporary uniforms and image bindings. They do not edit configuration or recompile programs.

### Transitions

An optional fifth argument controls animation:

```lua
return afterglow.shader_parameters(ev.pane, instance.instance,
    { strength = 0.8 }, "tab", { duration_ms = 650, curve = "ease-in-out" })
```

Duration is 0–60000 ms. Zero applies the change immediately.
Curves are `linear`, `ease-in-out` and the critically damped `spring`.
Phosphor curve specs also work, such as `0.2,0,0,1` for a cubic Bézier or `spring:20,1` for a spring with angular frequency 20 and damping ratio 1.
Curves that overshoot are rejected, such as elastic and bounce curves and springs with damping below 1.
Duration is also the maximum spring lifetime.
A new target starts from the visible value with a new duration.
Float parameters stay between their endpoints.
Colors interpolate in linear-light sRGB and return encoded color uniforms.
Integers, booleans and images change at completion.
Omitted parameters and empty-table resets release overrides immediately.
Unload, rebuild, pane closure and transfer cancel owned transitions.
Results confirm acceptance, not animation completion.
Hidden windows catch up when they render again.

[shader-parameters.luau](../data/examples/scripts/shader-parameters.luau) adds Bloom boost and reset actions.
Install it in `~/.config/afterglow/scripts/` and enable Bloom in a shader chain.
It targets the current tab, or the window for a window effect.
It reports when Bloom is absent.

### Presets

[scene-controls](../data/examples/packages/scene-controls/README.md) provides effect selection, presets, quality, intensity, save and reset controls.

| Helper | Result |
| --- | --- |
| `shader_presets(pane, instance)` | Available presets in `data.presets`. |
| `shader_preset(pane, instance, preset, scope?, transition?)` | Apply a preset with normal operation results. |
| `save_shader_preset(pane, instance, name)` | Save a preset. Return `data.id` and `data.name`. |

`@defaults` applies pack defaults. An empty `shader_parameters` table releases only the calling script's changes.
Saved presets use Phosphor's schema and registry in `~/.config/afterglow/shader-presets/surface/`.
This directory is separate from PlasmaZones.
Pack presets are read-only. Saves create new named entries that survive restarts.
Applied values are temporary. Apply the preset again after a restart.

### Scene state

`scene_state(pane)` returns `data.scene`:

| Field | Meaning |
| --- | --- |
| `tab_id` | Current tab in the anchor pane's window. |
| `history_rows` | Combined retained history in that tab. |
| `history_normalized` | `rows / (rows + 5000)`. |
| `sample_ms` | Monotonic sample time. |
| `audio_available` | Whether valid audio data is available. |
| `bass`, `mids`, `treble`, `overall` | Audio levels from 0 to 1. |

Samples remain cached for 100 ms. Tab changes invalidate the cache.
History uses the current tab even when the anchor pane is in a background tab.
Queries observe the shared analyzer without starting capture.
Unavailable or stale capture returns false availability and zero levels.
Scripts can sample with bounded timers. The host handles continuous FFT input and interpolation.
[contextual-scene](../data/examples/packages/contextual-scene/README.md) makes one decision each second and changes exposure with a smooth transition.
