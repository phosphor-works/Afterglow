// SPDX-License-Identifier: GPL-3.0-or-later
void mainImage(out vec4 color, in vec2 pixel) {
    vec2 uv = pixel / iResolution.xy;
    vec4 field = texture(iBuffer0, uv + vec2(0.01 * sin(iTime), 0.0));
    vec4 previous = texture(iBuffer1, uv);
    color = mix(field, previous, 0.85);
    depthBuffer = texture(iDepth, uv);
}
