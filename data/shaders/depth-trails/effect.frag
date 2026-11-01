// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include <terminal_multipass.glsl>

vec4 pTerminal(vec2 uv) {
    vec4 terminal = terminalTexel(uv);
    vec4 asset = texture(p_image, uv);
    vec3 field = mix(texture(iChannel1, uv).rgb, asset.rgb, asset.a);
    float depth = texture(uDepthBuffer, uv).r;
    // Clamped to alpha, so the colour stays valid premultiplied colour over a
    // translucent window.
    vec3 lit = terminal.rgb + field * (0.2 + depth * 0.8) * p_strength * terminal.a;
    return vec4(min(lit, vec3(terminal.a)), terminal.a);
}
