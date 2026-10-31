// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The previous frame's processed text layer, for TERMINAL packs that leave
// afterimages. Including this module is the opt-in: only then does Afterglow
// render layers separately, keep a history texture and redraw every frame.
// Sample it with the canvas's `uv`. It starts transparent, and layout, theme
// and shader changes clear it.

#ifndef AFTERGLOW_TERMINAL_HISTORY_GLSL
#define AFTERGLOW_TERMINAL_HISTORY_GLSL

#include <terminal_uniforms.glsl>

layout(binding = 17) uniform sampler2D uPreviousText;

#endif // AFTERGLOW_TERMINAL_HISTORY_GLSL
