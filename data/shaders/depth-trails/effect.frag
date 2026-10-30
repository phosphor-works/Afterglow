// SPDX-License-Identifier: GPL-3.0-or-later
void mainImage(out vec4 color, in vec2 pixel) {
    vec2 uv = pixel / iResolution.xy;
    vec4 terminal = texture(iChannel0, uv);
    vec4 asset = texture(uTexture0, uv);
    vec3 field = mix(texture(iBuffer1, uv).rgb, asset.rgb, asset.a);
    float depth = texture(iDepth, uv).r;
    color = vec4(terminal.rgb + field * (0.2 + depth * 0.8) * p_strength * terminal.a, terminal.a);
}
