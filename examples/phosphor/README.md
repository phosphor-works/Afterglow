<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Phosphor for Afterglow

This scene places a shared 3D reactor inside a lit chamber.
It includes curved metal fragments, plasma, magnetic filaments, haze and reflections.
System playback controls armor, plasma and lights.
Shell events charge and fracture the reactor. Shock fronts distort live terminal content.

![Phosphor reactor and terminal panes](preview.webp)

## Try it

From a built checkout, run:

```sh
examples/phosphor/bin/phosphor-afterglow launch --show --binary ./build/afterglow/afterglow
```

Add `--mesh` to use the triangle-mesh reactor with the same script commands:

```sh
examples/phosphor/bin/phosphor-afterglow launch --mesh --show --binary ./build/afterglow/afterglow
```

This mode adds segmented armor, plasma, orbiting emitters and a lit floor.
The default scene uses ray marching.

With an installed Afterglow, omit `--binary`. The launcher then runs the `afterglow` on `PATH`.
Requirements are Python 3, Bash, Wayland, Vulkan and the current Afterglow shader and Luau APIs.
Required interfaces include the terminal shader contract with `terminal_history.glsl`, `terminal_audio.glsl`, `highDynamicRange` and `uScrollback`, version 1 snapshots, parameter transitions and presets.
Install `cava` for audio response.
Add `--light` to start with the light palette.

The launcher creates temporary configuration, shader packs and scripts, then starts a separate Afterglow process.
When you close its last window, the launcher removes the temporary configuration.
The launcher does not change your normal Afterglow settings.
Each split opens a clean Bash demo shell with OSC 133 integration.
Programs inside it use your normal XDG configuration directory.
The demo shell does not save history.

## Demonstration commands

```sh
phosphor show
phosphor scene light
phosphor scene dark
```

`--show` starts the demonstration in the first pane. `phosphor show` repeats it.
The stages are idle, charge, fracture, recall, shock front and afterglow.
The sequence uses the alternate screen. Completion or Ctrl+C restores the prompt.
OSC 1337 user variables trigger the same steps as the script commands below.
A running shell command charges the core. A failed shell command fractures it.
A shell command that succeeds after 1.5 seconds or longer recalls its pieces.

## Scene controls

Select **Edit scene parameters and presets** in the command palette.
Select `background: phosphor-field` to apply Quiet, Balanced or Cinematic.
The editor also changes parameters, saves current values and resets its overrides.
Demo presets exist only until the launcher removes the temporary directory.
Install [scene-controls](../packages/scene-controls/README.md) in normal configuration for persistent presets.

**Toggle contextual Phosphor exposure** enables one intensity decision each second from audio and history summaries.
The host interpolates changes. Disable this option to release its overrides.
Audio and history continue to affect the shader directly.
Shell command activity uses host transitions. Fracture, recall and shock fronts use signal timestamps.

See [performance](#performance) before choosing a preset for 4K.
Quality tiers specify sampling quality, not guaranteed frame rates.
The text effect adds a dark glyph outline for readability over highlights.
`phosphor-emission.readability` controls this outline from 0 to 1. Its default is 0.9.

## Audio response

Play audio in your normal player or browser.
PhosphorAudio and CAVA analyze system playback, as in PlasmaZones.

| Input | Effect |
| --- | --- |
| Bass | Opens armor, increases plasma energy and strengthens floor caustics. |
| Mids | Bend and illuminate filaments and haze. |
| Treble | Illuminates floating particles. |
| Individual bands | Illuminate panel indicators and orbital segments. |

Audio and Luau operate together.
A failed shell command fractures the core. A success recalls it. Bells send shock fronts.
Silence preserves normal scene animation.
The lens adds bloom before text composition.

`Shaders.params.phosphor-field.audioStrength` ranges from 0 to 2. Its default is 1.
Set `Audio.enabled` to `false` to stop capture.
`Audio.inputMethod` and `Audio.inputSource` select the backend and source. Both default to `auto`.
Capture stops when no exposed audio-responsive window remains.
Missing CAVA logs a warning. The scene continues to run.

## Scrollback response

Combined retained history in the current tab increases the scene's baseline energy.
Effects include brighter plasma, more open armor, stronger violet light, illuminated filaments, haze and floor caustics.
Background tabs and other windows do not contribute.
Audio and Luau continue to apply their effects.

1. Run `seq 1 4000` in a demo pane.
2. Switch to a fresh tab.
3. Return to the original tab.

The fresh tab reduces baseline energy. Returning restores the original history contribution.
Clearing history or closing a pane lowers the total.
Full-screen programs preserve primary-screen history influence.

`Shaders.params.phosphor-field.historyStrength` ranges from 0 to 2. Its default is 1. Zero disables the influence.
`historyScale` sets the growth rate. The default is 4000 rows.
At this setting, 4000 combined rows produce approximately 63 percent influence. 10000 rows produce 92 percent.
The influence approaches its maximum smoothly.
Growth and decay use the renderer's smoothed count.

## Make a clip

1. Open the demo.
2. Enlarge the window.
3. Press **Ctrl+Shift+E** to split right.
4. Press **Ctrl+Shift+O** to split the focused pane down.
5. Move the pointer over the window to change depth.
6. Drag a divider to show different parts of the scene.
7. Start music.
8. Run `phosphor show` in one pane.

The core uses one camera across the window. The scene remains stationary while dividers move.
Quiet prompts in other panes make light and afterimages easier to see.
The final view can show the reassembled core and the Afterglow desktop icon.

The command palette (**Ctrl+Shift+P**) also contains these script commands:

| Shortcut | Palette title | Script command | Signal |
| --- | --- | --- | --- |
| Ctrl+Alt+1 | Phosphor: fracture the core | `strike` | 0 |
| Ctrl+Alt+2 | Phosphor: recall the fragments | `complete` | 1 |
| Ctrl+Alt+3 | Phosphor: send a shock front | `ring` | 3 |
| Ctrl+Alt+4 | Phosphor: charge the core | `heat` | 2 |

The charge lasts until the next shell command in the tab completes.
The script command `cool` ends the charge. It is not in the palette.

Use the palette if desktop shortcuts intercept these keys.
The demo is a separate process with its own socket, `afterglow-DISPLAY-PID.sock`.
`afterglow ctl` and `aglow run` in its panes reach it.
In its panes, the command `afterglow` runs the program of the demo, also from a build directory.
So does `aglow` when it is in the same directory as that `afterglow`.
For example, `aglow run phosphor strike` fractures the core.
`aglow run` is a shell command too, and its end cools the core of its tab.
For this reason, name a pane of another tab: `aglow run --pane N phosphor heat`.
The options of `aglow run` come before the script ID.
Elsewhere, use the palette or the shortcuts.

## Layers and signals

| Pack | Chain | Behavior |
| --- | --- | --- |
| `phosphor-field` | background | Curved shell panels, studio lighting, plasma volume, filaments, orbital traces, particles, haze, caustics and floor reflections. The plasma uses 48 steps. |
| `phosphor-lens` | background, after field | Three-scale HDR bloom, anamorphic scatter, filmic highlight compression, vignette and dithering. Runs before text composition. |
| `phosphor-emission` | text | Colored glyph halos and retained afterimages. Current glyphs preserve color and position. |
| `phosphor-cursor` | cursor | Tapered filaments during cursor movement. |
| `phosphor-frame` | pane | Frame light, refractive shock fronts, trailing ripples and a narrow color fringe. |

The script reads `ev.terminal` snapshots and updates the event's tab with `afterglow.signals`.
Each batch uses one timestamp across panes.
It also works when loaded after panes exist.
The script keeps the tab of each pane, from its load on.
As a result, a pane that moves to another tab updates the heat of the tab that it left.
One completed shell command does not cool the core while another pane in that tab runs a shell command.
Background tabs and other windows retain separate state.
Each signal stores a value and its shader update time.

| Channel | Meaning |
| --- | --- |
| 0 | Failed shell command exit code. Ctrl+C and unknown codes are ignored. Also 1 for program status `error`. |
| 1 | Successful shell command duration in seconds, for shell commands of at least 1.5 seconds. Also used for program status `done`. |
| 2 | 1 during a shell command. 0 after completion. |
| 3 | Bell event. |
| 4 | Program status: 1 working, 2 blocked, 3 error, 0 otherwise. |

## Rendering and memory

Text history uses one window-sized texture of the previous processed text layer.
Decay uses elapsed frame time.
Shader reload, layout, tab, size, default colors, font cell size and clock reset clear history.
Afterglow allocates and copies the history only when a shader includes `terminal_history.glsl`.
The plain renderer does not require it.

Field and lens request RGBA16F intermediate targets with `"highDynamicRange": true` in their metadata.
Highlights remain above display white until lens bloom and tone mapping.
Display output remains SDR.
The history texture requires approximately 16 MiB at 1080p or 64 MiB at 4K, in addition to layer targets.
Chains without this setting use RGBA8 at half that memory cost.

The scene combines analytic ray intersections, rounded signed-distance surfaces and volume integration.
Glyphs, selection and input remain on the flat terminal surface.
The renderer controls history and layer separation independently of the compositor.
Palette changes reload immediately without a color transition.

## Performance

These measurements describe one local Linux and Vulkan machine.
They do not certify other GPUs or compare Afterglow with other terminals.

| Condition | Value |
| --- | --- |
| CPU | Intel Core i7-9700K |
| GPU | NVIDIA RTX 2080, 8 GiB, driver 615.78.08 |
| System | CachyOS 7.3.0-4, Qt 6.12.0, KDE/KWin Wayland |
| Build type | `RelWithDebInfo` |
| Font | Monaspace Neon Var at 11 points, DPR 1 |
| Power | 225 W GPU limit, normal desktop power management, no locked clocks or exclusive GPU access |

`afterglow_renderpng` rendered exact 1920×1080 and 3840×2160 framebuffers.
Each scene used the same 120×40 terminal contents, a time of 10 seconds and simulated 30 Hz animation.
Synthetic bass, mids and treble were 0.5, 0.3 and 0.2. History was 5,000 rows.
Quiet, Balanced and Cinematic used shipped field presets and the complete lens, text, cursor and frame chains.
The no-shader case used equivalent terminal content.
The feedback case added feedback-glow to the text chain.
The mesh cases used the Mesh Reactor pack alone, then in linear light with the launcher's `--mesh` chains.
The two-pane case rendered two half-width panes that share one window background.
The mesh pass draws 13 instances of 73,728 vertices with 2× multisampling.

Each case rendered 32 frames and discarded the first timing.
Times include CPU submission and synchronous GPU readback. They exclude PNG encoding.
They do not measure GPU-only time, swapchain FPS, input latency or sustained power.
p95 is an observed short-run percentile, not a long-run guarantee.
`nvidia-smi` sampled process graphics allocations approximately ten times each second.
The GPU MiB columns give the largest observed sample, with driver bookkeeping and existing terminal targets.
The plain 1080p run produced only two memory samples.
The GPU also drove the desktop, at 21 to 39% utilization before the run.

| Case | 1080p p50 / p95 (ms) | 1080p GPU MiB | 4K p50 / p95 (ms) | 4K GPU MiB |
| --- | ---: | ---: | ---: | ---: |
| No shaders | 5.14 / 6.97 | 51 | 16.20 / 19.76 | 73 |
| Quiet | 18.17 / 20.85 | 195 | 57.33 / 61.37 | 585 |
| Balanced | 24.65 / 26.89 | 195 | 81.39 / 86.65 | 585 |
| Cinematic | 37.89 / 40.22 | 195 | 125.70 / 136.15 | 603 |
| Balanced + Feedback Glow on text | 25.78 / 28.58 | 207 | 82.35 / 89.04 | 621 |
| Mesh Reactor alone | 6.77 / 9.04 | 226 | 21.77 / 26.95 | 709 |
| Mesh scene (`--mesh` launcher chain) | 11.24 / 13.99 | 244 | 34.28 / 38.72 | 773 |
| Mesh scene, two panes | 11.62 / 16.33 | 308 | 35.22 / 42.52 | 1029 |

Quiet is the lowest-cost ray-marched preset.
Balanced and Cinematic use more ray samples for higher quality.
These results do not show a 4K 60 FPS scene on this GPU.
The mesh scene takes about 60% of the time of Quiet, because rasterized triangles replace per-pixel ray marching.
A second pane adds little time, because the shared background draws the mesh once per window.
It adds memory for the layer targets of the second pane.
Intensity changes appearance without reducing the main ray-marching cost.
Feedback adds persistent storage.
Observed driver memory exceeds the pixel-storage formula because the driver rounds allocations.

To measure on your machine, install Monaspace Neon Var and run this command from the repository root:

```sh
python3 tools/render/scene_benchmark.py build/tools/render/afterglow_renderpng /tmp/afterglow-scene-benchmark
```

With another font, the times are not comparable with the table.
The NVIDIA memory sampler is optional. Other GPUs give timings, with memory marked unavailable.
The tool does not substitute whole-machine GPU memory.
[Shader scenes](../../docs/TESTING.md#shader-scenes) gives the scene tests and their requirements.

The feedback case uses one buffer.
The mesh cases measure one depth-tested mesh pass.
They do not measure other multi-buffer effects, repeat or mirror wrapping, image parameters, vertex stages or auxiliary depth attachments.
Persistent buffer pixels have a 256 MiB limit per window.
Region inputs and uploaded images share a separate 256 MiB cache.
Driver allocations and frames in flight need more memory.
See the [shader pass API](../../docs/SHADER-API.md).

Luau summaries stay cached for 100 ms. Contextual exposure samples once each second.
Continuous motion and audio stay in the host and GPU.
Command and script bursts stay subject to the normal effect budget. Focus changes do not use that budget.

## Tuning

Each pack declares parameters in `metadata.json`.
Add overrides under `Shaders.params` in `config/phosphor.json` before launch:

```json
{
    "Shaders": {
        "params": {
            "phosphor-field": { "intensity": 0.85, "speed": 0.4, "parallax": 0.25, "quality": 1, "audioStrength": 1, "historyStrength": 1, "historyScale": 4000 },
            "phosphor-lens": { "bloom": 0.45, "streak": 0.12 },
            "phosphor-emission": { "persistence": 0.08, "glow": 0.18 },
            "phosphor-cursor": { "wake": 0.2 }
        }
    }
}
```

The shell contains 96 curved panels. Omitted panels expose the core.
Bounding spheres reject panels before surface ray marching.

| Quality | Samples per pixel |
| --- | --- |
| 0 | One. |
| 1 | Two rotated-grid samples. This is the default. |
| 2 | Four samples for capture. |

`shutter` controls motion integration across samples.
Text retains normal sharp rendering.
The scene animates continuously and traces reflected and primary views.
Cost increases with resolution and sample count.
Use quality 0 to reduce capture cost. Lens and atmosphere effects remain active.

Remove both `phosphor-field` and `phosphor-lens` from the background chain to disable the scene background.
Zero `speed` and `parallax` freeze the camera without stopping redraws from animated packs.
Clear all shader chains for reduced motion.

## Render an animation without desktop capture

The developer renderer accepts `--frames N --fps N`.
With multiple frames, `shot.png` becomes `shot-0000.png`, `shot-0001.png` and subsequent numbered files.
Shader time and signal ages advance at the requested playback rate, independently of render speed.
The renderer finds packs in the shader search path. The Phosphor packs are not in it.
Copy the packs, including `shared/`, into a separate configuration directory, and set `XDG_CONFIG_HOME` to it:

```sh
mkdir -p /tmp/phosphor-render/phosphor-works/afterglow/shaders
cp -r examples/phosphor/shaders/* /tmp/phosphor-render/phosphor-works/afterglow/shaders/
XDG_CONFIG_HOME=/tmp/phosphor-render afterglow_renderpng --background-shader phosphor-field \
    --background-shader phosphor-lens --dpr 2 --time 10 \
    --signal 0=1@0 --frames 48 --fps 24 input.ansi fracture.png
ffmpeg -framerate 24 -i fracture-%04d.png -c:v libx264 -pix_fmt yuv420p fracture.mp4
```

The preview uses the actual renderer.
`--timings --frames N` separates render/readback time from PNG encoding.
These measurements include readback. They are not GPU-only timings.

`--audio-bands 0.7,0.3,0.15` supplies synthetic bass, mid and treble levels for deterministic previews.
Without this option, the renderer uses silence.
The interactive launcher uses system playback automatically.

`--scrollback-lines 10000` supplies synthetic combined history.
Without this option, the renderer sums actual retained rows from the supplied panes.

Shell command activity uses `commandHeat` and a 650 ms host transition.
Fracture, recall and shock fronts use signal timestamps.
Install the shader pack and script together.
