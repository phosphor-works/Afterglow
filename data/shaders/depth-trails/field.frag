// SPDX-License-Identifier: GPL-3.0-or-later
void mainImage(out vec4 color, in vec2 pixel) {
    vec2 uv = pixel / iResolution.xy;
    float wave = 0.5 + 0.5 * sin(uv.x * 12.0 + sin(uv.y * 8.0 + iTime) * 2.0);
    color = vec4(vec3(0.08, 0.35, 0.7) * wave, 1.0);
    depthBuffer = vec4(wave, 0.0, 0.0, 1.0);
}
