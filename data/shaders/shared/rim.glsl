// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A rim, for packs that mark the edge of a pane or a card: a thin line at
// the edge of a rectangle, with a halo inside it. A pack that follows the
// Phosphor design language makes the rim bright at once when its rectangle
// changes, and then lets the halo fade, so it passes an energy that falls
// from 1 to 0.
//
// This file is part of each pack that includes it. Read no clock and no event
// age here: the pack would then render continuously.

#ifndef AFTERGLOW_RIM_GLSL
#define AFTERGLOW_RIM_GLSL

// How far inside the rectangle r (x, y, width, height) a pixel is, in pixels.
// Negative outside it.
float rimDistance(vec2 pixel, vec4 r) {
    return min(min(pixel.x - r.x, r.x + r.z - pixel.x), min(pixel.y - r.y, r.y + r.w - pixel.y));
}

// Adds light to a premultiplied colour. Alpha rises to cover the light, so
// the colour stays valid over a translucent window.
vec4 addLight(vec4 color, vec3 light) {
    float a = clamp(max(color.a, max(light.r, max(light.g, light.b))), 0.0, 1.0);
    return vec4(min(color.rgb + light, vec3(a)), a);
}

// Blends a premultiplied colour toward an opaque one: a line that covers the
// text under it, where addLight() would only brighten it.
vec4 coverWith(vec4 color, vec3 paint, float amount) {
    return mix(color, vec4(paint, 1.0), clamp(amount, 0.0, 1.0));
}

// Draws a rim over a premultiplied colour, `inside` pixels from the edge of
// its rectangle (rimDistance()): a line `width` pixels wide that covers what
// is under it by `line`, and a halo of strength `halo` that reaches `reach`
// pixels in. Nothing outside the rectangle.
vec4 drawRim(vec4 color, vec3 tint, float inside, float width, float line, float reach, float halo) {
    if (inside < 0.0) return color;
    color = addLight(color, tint * exp(-inside / max(reach, 1.0)) * halo * max(color.a, 0.5));
    return coverWith(color, tint, (1.0 - smoothstep(width - 0.5, width + 0.5, inside)) * line);
}

#endif // AFTERGLOW_RIM_GLSL
