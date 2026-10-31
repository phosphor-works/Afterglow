<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Mesh Reactor

Add `"mesh-reactor"` to `Shaders.background`.
To run it from a built checkout:

```sh
./build/afterglow/afterglow -o 'Shaders.background=["mesh-reactor"]'
```

The vertex shader generates a sphere and three rings from triangle grids.
One instanced draw renders 24,576 triangles with hardware depth testing.
Each object inherits the assembly transform and applies a local rotation.
A perspective camera projects the geometry. Surface normals control metal highlights and edge lighting.
The image pass adds a small glow and maps brightness to the display range.
Terminal text is drawn above the scene.

System audio changes the core brightness and ring thickness.
Current-tab scrollback increases the scene energy.
Use the scene parameter controls or Luau shader parameters to change `exposure`, `speed` and `audio`.
Silence does not stop the animation.

This pack demonstrates the mesh renderer. The Phosphor example keeps its ray-marched scene.
The pack uses approximately 47.5 MiB for buffers at 1080p and 189.9 MiB at 4K.
Other shader buffers share the same 256 MiB budget.

See [Mesh passes](../../../docs/SHADER-API.md#mesh-passes) for imported OBJ meshes,
vertex inputs, depth rules and resource limits.
