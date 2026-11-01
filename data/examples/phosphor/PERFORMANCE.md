<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Phosphor scene performance

Measurement date: 31 October 2026.
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
| Power | 225 W GPU limit. Normal desktop power management. No locked clocks or exclusive GPU access. |

`afterglow_renderpng` rendered exact 1920×1080 and 3840×2160 framebuffers.
Each scene used the same 120×40 terminal contents, time of 10 seconds and simulated 30 Hz animation.
Synthetic bass, mids and treble were 0.5, 0.3 and 0.2. History was 5,000 rows.
Quiet, Balanced and Cinematic used shipped field presets and the complete lens, text, cursor and frame chains.
The no-shader case used equivalent terminal content.
The optional feedback case added feedback-glow to the text chain.
The mesh cases used the Mesh Reactor pack: alone, then in linear light with the launcher's `--mesh` lens, text, cursor and frame chains.
The two-pane case rendered two half-width panes that share one window background.
The mesh pass draws 13 instances of 73,728 vertices with 2× multisampling.

Each main case rendered 32 frames and discarded the first timing.
Only the final PNG was saved.
Times include CPU submission and synchronous GPU readback. They exclude PNG encoding.
They do not measure GPU-only time, swapchain FPS, input latency or sustained power.
p95 is an observed short-run percentile, not a long-run guarantee.

`nvidia-smi` sampled process graphics allocations approximately ten times each second.
The table reports the largest observed sample. It is not an exact allocation trace.
Driver bookkeeping and existing terminal targets are included.
The plain 1080p run produced only two memory samples.
The GPU also drove the desktop, at 21–39% utilization before the run.
[validation-results.json](validation-results.json) contains exact sample counts and means.

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
These results do not demonstrate a 4K 60 FPS scene on this GPU.
The mesh scene takes about 60% of the time of Quiet, because rasterized triangles replace per-pixel ray marching.
A second pane adds little time, because the shared background draws the mesh once per window.
It adds memory for the second pane's layer targets.
Intensity changes appearance without reducing the main ray-marching cost.
Feedback adds persistent storage.
Observed driver memory exceeds the pixel-storage formula because allocations are rounded.
Short-run timing differences do not establish an optimization.

## Reproduce

Run from the repository root:

```sh
python3 tools/render/scene_benchmark.py build/tools/render/afterglow_renderpng /tmp/afterglow-scene-benchmark
```

The benchmark's NVIDIA memory sampler is optional.
Other GPUs still produce timings, with memory marked unavailable.
The tool does not substitute whole-machine GPU memory.
[Shader scenes](../../../docs/TESTING.md#shader-scenes) gives the scene tests and their requirements.

## Measurement limits

The optional feedback case uses one buffer.
The mesh cases measure one depth-tested mesh pass. The cases do not measure other multi-buffer effects, repeat/mirror wrapping, image parameters, vertex stages or auxiliary depth attachments.
Persistent buffer pixels have a 256 MiB limit per window.
Region inputs and uploaded images share a separate 256 MiB cache.
Driver allocations and frames in flight require additional memory.
See the [shader pass API](../../../docs/SHADER-API.md).

Luau summaries remain cached for 100 ms. Contextual exposure samples once each second.
Continuous motion and audio remain in the host and GPU.
Command and script bursts remain subject to the normal effect budget.
Focus changes do not consume that budget.
These measurements do not establish cross-terminal throughput or mixed-monitor performance.
