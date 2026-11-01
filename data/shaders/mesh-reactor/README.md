<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Mesh Reactor

Add `"mesh-reactor"` to `Shaders.background`.
To run it from a built checkout:

```sh
./build/afterglow/afterglow -o 'Shaders.background=["mesh-reactor"]'
```

The scene contains curved armour panels, a plasma core, three moving rings,
six orbiting emitters and a chamber floor. Hardware depth testing controls occlusion.
The metal has bevels, reflection highlights, engraved lines and status lights.
The final pass adds haze, particles, glow and display tone mapping.
Terminal text is drawn above the scene.
Cell backgrounds, selections and images below the text stay on top of the scene.
The scene shows where the window background shows, at the window's background opacity.
The camera spans the window. Each split pane shows its part of the same scene.
The mesh renders once per frame. Split animation does not resize the scene buffers.

System audio changes the core and lights. Current-tab scrollback increases
the baseline energy. Silence does not stop the animation.
The `exposure`, `speed`, `audio` and `commandHeat` parameters also accept Luau control.
Set `linearOutput` to `true` only when a later lens pass will map HDR radiance to the display.
The Phosphor launcher sets this parameter automatically.
Signal 0 fractures the armour. Signal 1 recalls it. The camera moves back during fracture.
Signal 3 sends a shock ring across the floor.

To use the complete Phosphor demo with its shell integration, text effects,
shock fronts and Luau actions, run:

```sh
data/examples/phosphor/bin/phosphor-afterglow launch --mesh --show --binary ./build/afterglow/afterglow
```

This launcher copies the current shader pack into a temporary demo configuration.
It does not use an older installed copy of the pack.

The mesh pass submits 319,488 triangles in one instanced draw.
It renders at 75 percent of the window framebuffer width and height and requests two-sample MSAA.
Devices that do not support two samples use one sample.
With two samples, its buffer budget is approximately 53.4 MiB at 1080p
and 213.6 MiB at 4K. Other shader buffers share the same 256 MiB budget.
These figures exclude the terminal targets, font atlas and driver allocations.
On an RTX 2080, the complete `--mesh` scene renders a 1080p frame in about 11 ms and a 4K frame in about 34 ms.
See [Phosphor scene performance](../../examples/phosphor/PERFORMANCE.md) for the conditions.
This is a triangle scene. The default Phosphor scene still uses ray marching.

See [Mesh passes](../../../docs/SHADER-API.md#mesh-passes) for imported OBJ meshes,
vertex inputs, depth rules and resource limits.
