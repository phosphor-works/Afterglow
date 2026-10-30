# V1 scene validation — 30 October 2026

V1.1–V1.5 are delivered: live named parameters, host transitions, Phosphor
presets and bounded telemetry, persistent terminal buffers, packaged controls,
and measured scene acceptance. This is a local Linux/Vulkan validation snapshot,
not a cross-GPU certification or a performance comparison with other terminals.

## Acceptance

- **340/340 CTest cases passed.** The renderer tests include live uniforms in
  every relevant chain, persistent-buffer accumulation in all five scopes,
  RGBA8 versus RGBA16F, disabled feedback, alpha/linear-light conversion,
  odd-sized split isolation at fractional buffer scale, reset on layout/tab/
  theme/resize/clock changes, and the buffer-instance bound.
- **Native preset flow passed:** keyboard-operated selector, apply, save, reset,
  restart persistence, stale-instance rejection and owner/uninstall cleanup.
  An intermittent selector timing failure was corrected in the smoke harness.
- **Native combined scene passed:** real CAVA capture from a temporary Pulse
  null-sink monitor, shell command heat transitioning up and down, and 9,944
  combined history rows across two panes. A fresh background-independent tab
  started with near-zero history; returning restored the first tab's total.
  Playback was routed only to the silent test sink, then stopped and removed.
- **Focus and lifecycle:** repeated focus changes, split resizing, pane zoom,
  fullscreen toggling, opacity/font reload, contextual exposure enable/reset,
  silence and disabled capture passed. The first combined run exposed redundant
  focus-triggered material updates; removing those updates eliminated budget
  drops. A regression sends 64 focus events without spending shader effects.
- **Unavailable capture:** native disabled capture reports `audio_available:
  false` and zero levels. Existing provider tests cover absent, failed and
  stalled capture with bounded retry behavior. No script performs FFT work.
- **Visual inspection:** dark and light scenes, 1×/1.25×/2× text scaling, split
  composition, 75% opacity, audio plus command impulses, and one million
  synthetic history rows rendered successfully. The bright-core test exposed
  white-on-white text; the text pass now adds a configurable dark outline behind
  glyph edges. The corrected split image retains the prompt over hot highlights.
  These DPR checks are offscreen raster checks; live mixed-monitor migration is
  not part of this snapshot.

The complete Phosphor launcher now includes the scene-controls and
contextual-scene packages. Its directory is temporary; saved presets disappear
when that demo closes. Install the packages in the normal configuration for
persistence across ordinary sessions. Reset releases the editor's contribution;
other scripts' contributions remain. Choosing Pack defaults explicitly applies
metadata defaults instead of the configured values.

## Measurement method

Host: Intel Core i7-9700K, NVIDIA RTX 2080 (8 GiB), driver 615.78.08,
CachyOS kernel 7.3.0-4, Qt 6.12.0, KDE/KWin Wayland. RelWithDebInfo build;
Monaspace Neon Var, renderer tool default 11-point font, DPR 1. GPU power limit
225 W; ordinary desktop power management, no locked clocks or exclusive GPU.

`afterglow_renderpng` rendered exact 1920×1080 and 3840×2160 framebuffers.
The same 120×40 terminal contents, time=10 seconds, 30 Hz simulated animation,
synthetic bass/mids/treble=.5/.3/.2 and 5,000 history rows were used in every
scene case. Quiet, Balanced and Cinematic use their shipped field presets and
the full lens/text/cursor/frame chains. No shaders is the equivalent terminal
baseline. The optional feedback case adds the reference pack on the text chain.

Each main case contains 32 frames, discarding the first, and saves only its final
PNG. Times include CPU submission and synchronous GPU readback, excluding PNG
encoding. **They are not GPU-only timings, swapchain FPS, input latency or
sustained-power measurements.** p95 is the observed short-run percentile, not a
long-run guarantee. `nvidia-smi` sampled process graphics allocations roughly
10 times per second; the table gives the largest observed sample, not an exact
allocation trace. Driver bookkeeping and existing terminal targets are included.
The plain 1080p run yielded only one memory sample. The exact sample counts and
means are in [validation-results.json](validation-results.json).

| Case | 1080p p50 / p95 (ms) | 1080p GPU MiB | 4K p50 / p95 (ms) | 4K GPU MiB |
| --- | ---: | ---: | ---: | ---: |
| No shaders | 4.28 / 6.56 | 51 | 10.78 / 13.29 | 73 |
| Quiet | 15.70 / 16.78 | 177 | 46.72 / 49.63 | 521 |
| Balanced | 21.96 / 22.97 | 177 | 68.41 / 71.84 | 521 |
| Cinematic | 33.52 / 35.90 | 177 | 112.68 / 121.54 | 521 |
| Balanced + Feedback glow on text | 22.55 / 23.90 | 189 | 69.30 / 72.96 | 557 |

Quiet has the lowest measured cost. Balanced and Cinematic deliberately trade
more ray samples for quality; none of these results supports advertising a 4K
60 FPS scene on this GPU. Reducing intensity changes the look, not the main
raymarch cost. Feedback adds persistent storage; the observed driver-memory
increase is larger than its pixel-storage formula because allocations are
rounded. Short-run timing differences should not be read as optimizations.

## Before/after adapter baseline

Before V1.4, the field-only quality=1 workload was recorded at the V1.3 renderer
(`33dc961`, with the measurement-tool changes). The exact workload was repeated
after the adapter integration: 12 frames, 11 measured. No feedback buffer is
allocated in this comparison; it checks the cost to existing shaders.

| Resolution | Before p50 / p95 (ms) | After p50 / p95 (ms) |
| --- | ---: | ---: |
| 1920x1080 | 17.93 / 22.64 | 21.13 / 22.92 |
| 3840x2160 | 59.43 / 63.73 | 59.53 / 61.93 |

These short runs show the same general cost range, with normal desktop/clock
variation. They do not establish a statistically significant improvement or
regression. Pre-adapter per-process GPU memory was not captured; the table above
supplies the final no-shader/scene memory baselines without inventing that value.

## Reproduce

From the repository root:

```sh
cmake -S . -B build -DAFTERGLOW_BUILD_TOOLS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
python3 afterglow/tests/shader_parameters_smoke.py build/afterglow/afterglow
python3 afterglow/tests/scene_acceptance.py build/afterglow/afterglow
python3 tools/render/scene_benchmark.py build/tools/render/afterglow_renderpng /tmp/afterglow-scene-benchmark
```

The native tests require KWin, X11/XWayland, XTest and ImageMagick. The combined
scene test also requires CAVA, pactl and paplay. The benchmark's NVIDIA memory
sampler is optional: other GPUs still produce render timings, with memory marked
unavailable. It does not silently substitute whole-machine GPU memory.

Local evidence from this run lives under `/tmp/afterglow-shader-parameters/`,
`/tmp/afterglow-scene-acceptance/` and `/tmp/afterglow-v1-evidence/`. These temporary
captures are not shipped assets; the scripts and summarized JSON are committed.

## Remaining limits

The measurements above describe the original single-buffer adapter. The
completion audit subsequently added up to eight buffers, repeat/mirror wrapping,
image parameters, custom vertex hooks and auxiliary depth attachments. The
recorded Phosphor timings have not been reinterpreted as measurements of those
new stages. Buffer pixels retain a 256 MiB per-window budget; region-local
inputs and uploaded images have a separate 256 MiB cache budget. Driver and
in-flight allocations are additional.
See the [buffer contract](../../shaders/feedback-glow/README.md).

Luau summaries are cached for 100 ms; contextual exposure samples once a second.
Continuous motion and audio remain in the host/GPU. Arbitrarily large bursts of
commands or script actions remain subject to the normal effect budget; the
focus path no longer consumes it. No cross-terminal throughput, accessibility,
packaging or mixed-monitor compatibility claims are implied by V1 completion.
