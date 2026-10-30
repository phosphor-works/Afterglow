<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Phosphor scene performance

Measurement date: 30 October 2026.
Results describe one local Linux/Vulkan environment.
They do not certify other GPUs or compare Afterglow with other terminals.

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

## Measurement limits

The optional feedback case uses one buffer.
These timings do not measure multi-buffer effects, repeat/mirror wrapping, image parameters, vertex hooks or auxiliary depth attachments.
Persistent buffer pixels have a 256 MiB limit per window.
Region inputs and uploaded images share a separate 256 MiB cache.
Driver allocations and frames in flight require additional memory.
See the [shader pass API](../../../docs/SHADER-API.md).

Luau summaries remain cached for 100 ms. Contextual exposure samples once each second.
Continuous motion and audio remain in the host and GPU.
Command and script bursts remain subject to the normal effect budget.
Focus changes do not consume that budget.
These measurements do not establish cross-terminal throughput or mixed-monitor performance.
