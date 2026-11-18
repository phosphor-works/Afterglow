// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The Phosphor spectrum, for the Phosphor packs and for packs that want the
// same look: the four brand colours (cyan, blue, purple, rose) as one
// gradient, and the rim that carries it. A rim is a thin line at the edge of
// a rectangle with a halo inside it. The line is dim while nothing changes.
// When its rectangle changes, the rim is bright at once and its halo fades
// slowly, so a pack passes an energy that falls from 1 to 0.
//
// This file is part of each pack that includes it. Read no clock and no event
// age here: the pack would then render continuously.
//
// A pack that includes this file declares these parameters:
//   followTheme  bool   take the colours from the terminal palette
//   stop0-stop3  color  the gradient, from its start to its end

#ifndef PHOSPHOR_SPECTRUM_GLSL
#define PHOSPHOR_SPECTRUM_GLSL

// The gradient's colours, sRGB. With followTheme they are the theme's bright
// cyan, blue, magenta and red, so a palette from a wallpaper carries through.
vec3 spectrumStop(int i) {
    if (p_followTheme > 0.5) {
        const int index[4] = int[4](14, 12, 13, 9);
        return paletteColor(index[clamp(i, 0, 3)]);
    }
    return i <= 0 ? p_stop0.rgb : i == 1 ? p_stop1.rgb : i == 2 ? p_stop2.rgb : p_stop3.rgb;
}

// The gradient at t, 0 to 1, sRGB. The stops are at 0, 0.34, 0.68 and 1, as
// in the Phosphor design language.
vec3 spectrum(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 c = mix(spectrumStop(0), spectrumStop(1), clamp(t / 0.34, 0.0, 1.0));
    c = mix(c, spectrumStop(2), clamp((t - 0.34) / 0.34, 0.0, 1.0));
    return mix(c, spectrumStop(3), clamp((t - 0.68) / 0.32, 0.0, 1.0));
}

// The colour that marks where a pane's shell runs (paneContext()): cyan for
// a remote host, rose for an elevated user, purple for a container.
vec3 spectrumContext(int context) {
    return context == 2 ? spectrumStop(3) : context == 3 ? spectrumStop(2) : spectrumStop(0);
}

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

#endif // PHOSPHOR_SPECTRUM_GLSL
