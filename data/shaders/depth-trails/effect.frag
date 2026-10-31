// SPDX-License-Identifier: GPL-3.0-or-later
#include <terminal_multipass.glsl>

vec4 pTerminal(vec2 uv) {
    vec4 terminal = terminalTexel(uv);
    vec4 asset = texture(p_image, uv);
    vec3 field = mix(texture(iChannel1, uv).rgb, asset.rgb, asset.a);
    float depth = texture(uDepthBuffer, uv).r;
    return vec4(terminal.rgb + field * (0.2 + depth * 0.8) * p_strength * terminal.a, terminal.a);
}
