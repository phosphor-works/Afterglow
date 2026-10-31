// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Colour-space helpers for TERMINAL shader packs. The terminal input and the
// final output are encoded sRGB with premultiplied alpha, and nothing converts
// them for you. Buffer contents carry no transfer function of their own, so a
// pack that accumulates light converts to linear on the way in and back on the
// way out (see feedback-glow).

#ifndef AFTERGLOW_TERMINAL_COLOR_GLSL
#define AFTERGLOW_TERMINAL_COLOR_GLSL

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow(max((c + 0.055) / 1.055, vec3(0.0)), vec3(2.4)), step(vec3(0.04045), c));
}

vec3 linearToSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// Premultiplied forms. Fully transparent colours convert to zero rather than
// dividing by zero.
vec4 toLinearPremultiplied(vec4 c) {
    return c.a > 0.0 ? vec4(srgbToLinear(c.rgb / c.a) * c.a, c.a) : vec4(0.0);
}

vec4 toSrgbPremultiplied(vec4 c) {
    return c.a > 0.0 ? vec4(linearToSrgb(c.rgb / c.a) * c.a, c.a) : vec4(0.0);
}

#endif // AFTERGLOW_TERMINAL_COLOR_GLSL
