// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Draws the orrery over the terminal's background layer, in encoded sRGB.
// Text is a separate layer and stays on top.
#include <terminal_multipass.glsl>

vec4 pTerminal(vec2 uv) {
    vec4 background = terminalTexel(uv);
    vec4 scene = texture(iChannel0, uv); // linear light, premultiplied
    vec3 color = scene.a > 0.0 ? linearToSrgb(min(scene.rgb / scene.a * p_brightness, vec3(1.0))) : vec3(0.0);
    float cover = scene.a * 0.9;
    return vec4(color * cover + background.rgb * (1.0 - cover), max(background.a, cover));
}
