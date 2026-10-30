<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Feedback glow

This example retains bright pixels in a half-resolution buffer with linear light.
The image pass adds an afterimage and preserves current glyphs.

Add `"feedback-glow"` to `Shaders.pane`.
Use `Shaders.text` instead to retain only glyphs.
Enable [scene-controls](../../examples/packages/scene-controls/README.md) for presets, intensity, decay, save and reset.

`strength: 0` preserves the input appearance. It does not stop animation or reduce buffer memory.
Remove the shader from configuration to stop its frame loop and release GPU resources.

See the [shader pass API](../../../docs/SHADER-API.md) for the complete buffer contract.
See [Phosphor](../../examples/phosphor/README.md) for the full scene.
