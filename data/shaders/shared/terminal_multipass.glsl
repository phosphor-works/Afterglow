// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Buffer-pass inputs for multipass TERMINAL packs (metadata `bufferShaders`).
// iChannelN is buffer pass N. Passes run in metadata order: in a buffer pass an
// earlier channel holds this frame's result, and the pass's own and later
// channels hold the previous frame (cleared each frame unless `bufferFeedback`
// keeps it). The final pass sees every buffer's current frame. Sample them
// with the same top-down `uv` as everything else.
//
// uDepthBuffer is the auxiliary output of the preceding pass when the pack
// enables `depthBuffer`: a buffer pass writes it through `depthBuffer`, and the
// first pass reads its own previous frame.

#ifndef AFTERGLOW_TERMINAL_MULTIPASS_GLSL
#define AFTERGLOW_TERMINAL_MULTIPASS_GLSL

#include <terminal_uniforms.glsl>

layout(binding = 2) uniform sampler2D iChannel0;
layout(binding = 3) uniform sampler2D iChannel1;
layout(binding = 4) uniform sampler2D iChannel2;
layout(binding = 5) uniform sampler2D iChannel3;
layout(binding = 6) uniform sampler2D iChannel4;
layout(binding = 7) uniform sampler2D iChannel5;
layout(binding = 8) uniform sampler2D iChannel6;
layout(binding = 9) uniform sampler2D iChannel7;

layout(binding = 16) uniform sampler2D uDepthBuffer;

#endif // AFTERGLOW_TERMINAL_MULTIPASS_GLSL
