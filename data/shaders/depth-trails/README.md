<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Depth Trails

Add `"depth-trails"` to `Shaders.background` to test the terminal shader stages.
This is an API example. [Phosphor](../../examples/phosphor/README.md) provides a complete scene.

The first buffer generates a moving field and auxiliary depth.
The second reads that field and depth, blends its previous frame and forwards depth.
The image pass uses `uDepthBuffer` to light the field.
A `pVertex` stage pulses the size of an oversized quad.
The quad's size changes which pixels run. It does not move the content, because the final pass computes `uv` from the pixel position.
Mirror sampling keeps the first buffer's shifted edge continuous without sampling another pane.

Enable [scene-controls](../../examples/packages/scene-controls/README.md) for the **Edit scene parameters and presets** command.
Use it to change intensity or enter an image path.
Paths can be pack-relative, absolute or begin with `~/`.
An empty path restores a transparent image.
Images change at transition completion and can be saved in presets.
Each image permits 16 MiB encoded data, 4096 pixels per side and four megapixels decoded.

Two half-resolution RGBA16F passes with feedback and auxiliary depth use approximately 31.6 MiB at 1080p or 126.6 MiB at 4K.
Remove the pack to release buffers and stop animation.
See the [shader pass API](../../../docs/SHADER-API.md) for pass order, coordinates, reset rules and shared memory limits.
