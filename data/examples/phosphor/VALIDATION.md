<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# V1 scene validation

Validation date: 30 October 2026.
V1.1–V1.5 include live parameters, host transitions, presets, bounded telemetry, persistent buffers, packaged controls and measured scene acceptance.
Results describe one local Linux/Vulkan environment.
They do not certify other GPUs or compare Afterglow with other terminals.

## Acceptance

| Check | Result |
| --- | --- |
| CTest | 340/340 passed. |
| Native presets | Selection, apply, save, reset, restart persistence, stale-ID rejection and owner/removal cleanup passed. |
| Combined scene | Real CAVA capture, command transitions and combined history passed. |
| Focus and lifecycle | Focus, split resize, zoom, fullscreen, opacity, font reload, contextual exposure, silence and disabled capture passed. |
| Disabled capture | Reports `audio_available: false` and zero levels. |
| Visual inspection | Dark and light scenes, text scaling, splits, opacity, audio, command impulses and large synthetic history rendered successfully. |

Renderer tests cover live uniforms in all relevant chains and retained buffers in all five scopes.
They also cover RGBA8, RGBA16F, disabled feedback, alpha conversion, linear-light conversion and fractional-scale split isolation.
Reset tests cover layout, tab, theme, resize and clock changes. Tests also enforce the buffer-instance limit.

A selector timing failure was corrected in the native test driver.
The combined test used a temporary Pulse null-sink monitor and 9,944 history rows across two panes.
A fresh tab started with near-zero history. Returning restored the original tab's total.
Playback used only the silent test sink. The test then stopped playback and removed the sink.

The first combined run found redundant focus-triggered material updates.
Removing them eliminated effect-budget drops.
A regression sends 64 focus events without spending shader effects.
Provider tests cover absent, failed and stalled capture with bounded retries.
Scripts do not calculate FFT data.

Visual checks used text scales of 1×, 1.25× and 2×, 75 percent opacity and one million synthetic history rows.
The bright-core check found white text over white highlights.
The corrected text pass adds a configurable dark outline and preserves the prompt.
DPR checks used offscreen rendering. They did not test live mixed-monitor migration.

The launcher includes scene-controls and contextual-scene packages.
Saved demo presets disappear when its temporary directory is removed.
Install packages in normal configuration for persistence.
Reset releases only the editor's contribution.
Pack defaults apply metadata defaults, not configured values.

## Measurement method

| Item | Value |
| --- | --- |
| CPU | Intel Core i7-9700K. |
| GPU | NVIDIA RTX 2080, 8 GiB. |
| Driver | 615.78.08. |
| Kernel | CachyOS 7.3.0-4. |
| Desktop | Qt 6.12.0, KDE/KWin Wayland. |
| Build | RelWithDebInfo. |
| Font | Monaspace Neon Var, renderer default 11-point size, DPR 1. |
| Power | 225 W GPU limit; normal desktop power management. No locked clocks or exclusive GPU access. |

`afterglow_renderpng` rendered exact 1920×1080 and 3840×2160 framebuffers.
Each scene used the same 120×40 terminal contents, time of 10 seconds and simulated 30 Hz animation.
Synthetic bass, mids and treble were 0.5, 0.3 and 0.2. History was 5,000 rows.
Quiet, Balanced and Cinematic used shipped field presets and the complete lens, text, cursor and frame chains.
The no-shader case used equivalent terminal content.
The optional feedback case added feedback-glow to the text chain.

Each main case rendered 32 frames and discarded the first timing.
Only the final PNG was saved.
Times include CPU submission and synchronous GPU readback. They exclude PNG encoding.
They do not measure GPU-only time, swapchain FPS, input latency or sustained power.
p95 is an observed short-run percentile, not a long-run guarantee.

`nvidia-smi` sampled process graphics allocations approximately ten times each second.
The table reports the largest observed sample. It is not an exact allocation trace.
Driver bookkeeping and existing terminal targets are included.
The plain 1080p run produced only one memory sample.
[validation-results.json](validation-results.json) contains exact sample counts and means.

| Case | 1080p p50 / p95 (ms) | 1080p GPU MiB | 4K p50 / p95 (ms) | 4K GPU MiB |
| --- | ---: | ---: | ---: | ---: |
| No shaders | 4.28 / 6.56 | 51 | 10.78 / 13.29 | 73 |
| Quiet | 15.70 / 16.78 | 177 | 46.72 / 49.63 | 521 |
| Balanced | 21.96 / 22.97 | 177 | 68.41 / 71.84 | 521 |
| Cinematic | 33.52 / 35.90 | 177 | 112.68 / 121.54 | 521 |
| Balanced + Feedback glow on text | 22.55 / 23.90 | 189 | 69.30 / 72.96 | 557 |

Quiet has the lowest measured cost.
Balanced and Cinematic use more ray samples for higher quality.
These results do not demonstrate a 4K 60 FPS scene on this GPU.
Intensity changes appearance without reducing the main ray-marching cost.
Feedback adds persistent storage.
Observed driver memory exceeds the pixel-storage formula because allocations are rounded.
Short-run timing differences do not establish an optimization.

## Before/after adapter baseline

The pre-V1.4 baseline used the V1.3 renderer at `33dc961`, with measurement-tool changes.
The field-only workload used quality 1.
The same workload ran after adapter integration: 12 frames, with 11 measured.
This comparison allocates no feedback buffer. It checks cost for existing shaders.

| Resolution | Before p50 / p95 (ms) | After p50 / p95 (ms) |
| --- | ---: | ---: |
| 1920x1080 | 17.93 / 22.64 | 21.13 / 22.92 |
| 3840x2160 | 59.43 / 63.73 | 59.53 / 61.93 |

Results remain in the same general cost range with normal desktop and clock variation.
They do not establish a statistically significant improvement or regression.
Pre-adapter process GPU memory was not recorded.
The first table supplies final no-shader and scene memory values.

## Reproduce

Run from the repository root:

```sh
cmake -S . -B build -DAFTERGLOW_BUILD_TOOLS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
python3 afterglow/tests/shader_parameters_smoke.py build/afterglow/afterglow
python3 afterglow/tests/scene_acceptance.py build/afterglow/afterglow
python3 tools/render/scene_benchmark.py build/tools/render/afterglow_renderpng /tmp/afterglow-scene-benchmark
```

Native tests require KWin, X11 or XWayland, XTest and ImageMagick.
The combined scene test also requires CAVA, pactl and paplay.
The benchmark's NVIDIA memory sampler is optional.
Other GPUs still produce timings, with memory marked unavailable.
The tool does not substitute whole-machine GPU memory.

Local evidence used `/tmp/afterglow-shader-parameters/`, `/tmp/afterglow-scene-acceptance/` and `/tmp/afterglow-v1-evidence/`.
Temporary captures are not shipped assets. Scripts and summarized JSON are committed.

## Remaining limits

These measurements describe the original single-buffer adapter.
The later completion audit added eight buffers, repeat/mirror wrapping, images, vertex hooks and auxiliary depth attachments.
The recorded Phosphor timings do not measure these new stages.
Persistent buffer pixels have a 256 MiB limit per window.
Region inputs and uploaded images share a separate 256 MiB cache.
Driver allocations and frames in flight require additional memory.
See the [shader pass API](../../../docs/SHADER-API.md).

Luau summaries remain cached for 100 ms. Contextual exposure samples once each second.
Continuous motion and audio remain in the host and GPU.
Command and script bursts remain subject to the normal effect budget.
Focus changes do not consume that budget.
V1 results do not establish cross-terminal throughput, accessibility, packaging or mixed-monitor compatibility.
