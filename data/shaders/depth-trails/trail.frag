// SPDX-License-Identifier: GPL-3.0-or-later
#include <terminal_multipass.glsl>

vec4 pTerminal(vec2 uv) {
    vec4 field = texture(iChannel0, uv + vec2(0.01 * sin(iTime), 0.0));
    vec4 previous = texture(iChannel1, uv);
    depthBuffer = texture(uDepthBuffer, uv);
    return mix(field, previous, 0.85);
}
