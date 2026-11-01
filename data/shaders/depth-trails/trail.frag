// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include <terminal_multipass.glsl>

vec4 pTerminal(vec2 uv) {
    vec4 field = texture(iChannel0, uv + vec2(0.01 * sin(timeAngle(1.0)), 0.0));
    vec4 previous = texture(iChannel1, uv);
    depthBuffer = texture(uDepthBuffer, uv);
    // Keep 85% of the trail per 60 Hz frame, at any frame rate.
    return mix(field, previous, pow(0.85, min(iTimeDelta, 1.0) * 60.0));
}
