<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Phosphor for Afterglow

A shared 3D reactor in a lit chamber: curved metal fragments, volumetric plasma,
magnetic filaments, turbulent haze and reflections. System playback drives the
armor, plasma and lights; shell events charge and
fracture the reactor, then send refractive waves through live terminal content.

![The Phosphor reactor charging, fracturing and recalling its shell in a reflective chamber](preview.webp)

## Try it

From a built checkout:

```sh
data/examples/phosphor/bin/phosphor-afterglow launch --show --binary ./build/afterglow/afterglow
```

After installation:

```sh
/usr/share/afterglow/examples/phosphor/bin/phosphor-afterglow launch --show
```

Adjust the prefix if installed somewhere other than `/usr`. Requires Python 3,
Bash, Wayland, Vulkan, and an Afterglow build with `iPreviousText`, `AFTERGLOW_HDR`
and `AFTERGLOW_AUDIO` support, plus the `iScrollback` uniform. Install `cava` for the audio response.
Add `--light` to start in the light palette.

The launcher creates a temporary configuration, shader packs and Luau script,
then starts a separate Afterglow process. Closing its last window removes that
configuration. Your normal Afterglow settings stay intact. Every split opens a
clean Bash demo shell with OSC 133 integration; programs started inside it see
your usual XDG configuration directory. The demo shell does not save history.

In the demo:

```sh
phosphor show
phosphor scene light
phosphor scene dark
```

`--show` plays the showcase automatically in the first pane. `phosphor show`
replays it: idle, charge, fracture, recall, impulse, then afterglow. The sequence
uses the alternate screen and restores your prompt on completion or Ctrl+C.
It drives the real Luau actions via OSC 1337 user variables. Ordinary commands
also charge the core, fracture it on failure and recall its pieces on success.

## Audio response

Play music in your usual player or browser. The scene follows system playback
through the same PhosphorAudio/CAVA provider as PlasmaZones:

- Bass opens the armor, energizes the plasma volume and strengthens floor caustics.
- Mids bend and illuminate the magnetic filaments and surrounding haze.
- Treble lights the floating motes; individual bands illuminate panel indicators
  and segments of the orbital traces.

Audio and Luau run together: command failures still fracture the core, success
recalls it, and bells send shock fronts. Silence retains the normal scene.
The lens blooms the changing emission before text is composited.

`Shaders.params.phosphor-field.audioStrength` controls the response (0–2,
default 1). Set `Audio.enabled` to `false` to stop capture entirely. The
`Audio.inputMethod` and `Audio.inputSource` settings select a backend and
source; both default to `auto`. Capture stops when no exposed audio-reactive
window remains. Missing CAVA logs a warning and leaves the scene running.

## Scrollback response

The combined retained history of all panes in the **current tab** gradually
builds the scene's baseline: brighter and more intricate plasma, slightly opened
armor, stronger violet energy, illuminated magnetic filaments, haze and floor
caustics. Background tabs and other windows do not contribute. Audio and Luau
keep driving their effects on top of this baseline.

Try `seq 1 4000` in a demo pane. Switch to a fresh tab to see the scene ease back
down; return to the original tab to restore its accumulated energy. Clearing
history or closing a pane also lowers the total. Entering a full-screen program
preserves the primary screen's history contribution.

`Shaders.params.phosphor-field.historyStrength` sets the influence (0–2,
default 1; zero disables it). `historyScale` controls how quickly it builds:
at the default 4000 rows, the effect reaches about 63% at 4000 combined rows and
92% at 10000. It saturates smoothly for long sessions, keeping text crisp and
music transients distinct. Growth and decay use the renderer's eased count.

## Make a clip

1. Open the demo and enlarge its window.
2. Use **Ctrl+Shift+E** to split right, then **Ctrl+Shift+O** to split the
   focused pane down. The containment core has one camera for the whole window.
3. Move the pointer over the window to see depth, and drag a divider: the scene
   stays in place while the panes expose different parts of it.
4. Start music, then run `phosphor show` in one pane. Keep the others at a quiet prompt so the
   light and afterimages have room to read.
5. End on the reassembled core with the new Afterglow icon visible in the desktop.

The command palette (**Ctrl+Shift+P**) also has four Phosphor actions:

| Shortcut | Action | Signal |
|---|---|---|
| Ctrl+Alt+1 | Fracture the core | 0 |
| Ctrl+Alt+2 | Recall the fragments | 1 |
| Ctrl+Alt+3 | Send a shock front | 3 |
| Ctrl+Alt+4 | Charge until the next command finishes | 2 |

Desktop shortcuts may intercept these; use the palette in that case. The
isolated process does not own the ordinary `afterglow ctl` service name, so
use these in-process actions when trying the demo alongside your normal app.

## Layers and signals

| Pack | Chain | Behavior |
|---|---|---|
| `phosphor-field` | background | Rounded spherical shell segments, reflected studio lighting, a 48-step plasma volume, magnetic filaments, orbital traces, defocused motes, turbulent haze, caustic floor light and ray-traced floor reflections. |
| `phosphor-lens` | background, after field | HDR highlight bloom at three scales, anamorphic scatter, filmic highlight rolloff, vignette and fine dithering. Runs before glyphs are composited. |
| `phosphor-emission` | text | A small coloured halo around glyphs and persistent text afterimages; current glyphs retain their original colour and position. |
| `phosphor-cursor` | cursor | A tapered filament on cursor movement. |
| `phosphor-frame` | pane | Frame light plus a refractive pressure front and trailing ripples through live terminal content, with a narrow colour fringe. |

The Luau script uses the existing `afterglow.signal` API and broadcasts events
to known panes in the same window. A command finishing does not cool the core
while another pane is still running. Separate windows keep their own state. Channels contain a
value and the shader time at which it was set:

| Channel | Meaning |
|---|---|
| 0 | Failed command's exit code; Ctrl+C and unknown exit codes are ignored |
| 1 | Successful command's duration in seconds, for commands lasting at least 1.5 seconds; also program status `done` |
| 2 | 1 while a command runs, 0 when it ends |
| 3 | Bell event |
| 4 | Program status: 1 working, 2 blocked, 3 error, 0 otherwise |

The text history is a single window-sized texture of the previous processed
text layer. Decay uses elapsed frame time. History clears on shader
reload, pane layout/tab changes, resize, default foreground/background colour
changes, font cell-size changes, and clock resets. It is allocated and copied
only when a shader references `iPreviousText`. The plain renderer is unchanged.
The field and lens request RGBA16F intermediate targets with
`#define AFTERGLOW_HDR 1`. Highlights stay above display white until the lens
pass blooms and tone-maps them. This is an SDR presentation with HDR
intermediates. The history texture costs about 16 MiB at 1080p or 64 MiB at 4K
in this mode, in addition to the layer targets. Chains without this opt-in
retain RGBA8 targets at half that memory cost.

The scene combines analytic ray intersections, rounded signed-distance surfaces
and volume integration. It is a 3D background; glyphs,
selection and input remain on the normal flat terminal surface. The frame
history and layer separation work in the renderer, independently of the
compositor. Palette changes currently reload immediately rather than morphing.

## Tuning

Parameters are declared in each pack's `metadata.json`. Add overrides under
`Shaders.params` in `config/phosphor.json` before launching, for example:

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

The shell is divided into 96 curved panels, with selected panels omitted to
expose the core. Bounding spheres reject panels before marching their rounded
surfaces. The default `quality: 1` uses two rotated-grid samples per pixel;
`quality: 2` uses four for capture, and `quality: 0` uses one. `shutter` controls motion
integration across those samples. Text keeps its normal sharp rendering.

This scene animates continuously and traces reflections as well as the primary
view. Cost grows with resolution and sample count. Use `quality: 0` if
recording at high resolution is too expensive; the lens and atmosphere remain.
To disable the scene, remove **both** `phosphor-field` and `phosphor-lens` from
the background chain. A zero `speed` and `parallax` freeze the camera, but do not
disable redraws while animated packs remain enabled. For reduced motion, clear
all shader chains.

## Render an animation without recording the desktop

The developer renderer accepts `--frames N --fps N`; with multiple frames,
`shot.png` becomes `shot-0000.png`, `shot-0001.png`, and so on. Shader time and
signal ages advance at the requested playback rate, independently of render
speed. For example, with these packs in the renderer's shader search path:

```sh
afterglow_renderpng --background-shader phosphor-field \
    --background-shader phosphor-lens --dpr 2 --time 10 \
    --signal 0=1@0 --frames 48 --fps 24 input.ansi fracture.png
ffmpeg -framerate 24 -i fracture-%04d.png -c:v libx264 -pix_fmt yuv420p fracture.mp4
```

The preview is generated through this real renderer, rather than a mockup.

Use `--timings --frames N` to separate render/readback time from PNG encoding.
These wall-clock measurements include readback and are not GPU-only timings.

For deterministic audio previews, add `--audio-bands 0.7,0.3,0.15` to the
render command. These are synthetic bass/mid/treble levels, not live capture.
Omitting the option renders silence. The interactive launcher uses real
system playback automatically.

Add `--scrollback-lines 10000` for a synthetic combined-history preview. Without
this option the renderer sums the actual retained rows in the supplied panes.
