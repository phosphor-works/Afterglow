// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Phosphor Panes: panes opening from a split and closing, in the Phosphor
// design language. Nothing turns or bends: a position moves on the window's
// own split animation, and this pack changes only light.
//   open     the pane's picture fades in from its background, and its rim
//            carries the spectrum, bright at once and then fading
//   close    the pane's last frame fades out over what took its place, with
//            the same rim
//   context  a pane whose shell moves to another host, user or container
//            (paneContext()) keeps a thin line at its edge in the colour of
//            that place: cyan remote, rose elevated, purple a container. The
//            line is bright at the change and then settles
//   pulses   afterglow.pulse(): a ring, a flash at the rim or a shake
// The input holds every open pane where it is; a closed pane's last frame
// comes from closingPaneTexel() at closingPaneRect(). Every progress is
// linear, and the pack eases it. At progress 1 nothing changes.

#include <terminal_panes.glsl>
#include <phosphor_spectrum.glsl>

bool inside(vec2 p, vec4 r) {
    return all(greaterThanEqual(p, r.xy)) && all(lessThan(p, r.xy + r.zw));
}

// A window pixel clamped half a pixel inside r, so the linear sampler takes
// nothing from the neighbouring pane or divider.
vec2 clampToRect(vec2 pixel, vec4 r) {
    return clamp(pixel, r.xy + 0.5, r.xy + r.zw - 0.5);
}

// A pane with no picture yet: its own background.
vec4 paper() {
    return vec4(uBackgroundColor.rgb, 1.0) * uBackgroundColor.a;
}

// The spectrum across the rectangle, from its left edge to its right.
vec3 across(vec2 pixel, vec4 r) {
    return spectrum((pixel.x - r.x) / max(r.z, 1.0));
}

// The light of a change that has run to p: all of it at once, then a slow fade.
float fading(float p) {
    float left = 1.0 - clamp(p, 0.0, 1.0);
    return left * left;
}

vec4 closing(vec2 pixel, vec4 color) {
    if (!closingPaneActive()) return color;
    vec4 r = closingPaneRect();
    if (!inside(pixel, r)) return color;
    float p = closingPaneProgress();
    vec4 ghost = closingPaneTexel(clampToRect(pixel, r) / iResolution);
    color = mix(color, ghost, 1.0 - smoothstep(0.0, p_fade, p));
    float energy = fading(p);
    return drawRim(color, across(pixel, r), rimDistance(pixel, r), p_rimWidth, p_rim * energy, p_reach, p_glow * energy);
}

vec4 opening(vec2 pixel, vec4 color) {
    for (int i = 0; i < paneCount(); ++i) {
        float p = paneOpening(i);
        if (p >= 1.0 || paneOpenEdge(i) == 0 || !paneInCurrentTab(i)) continue;
        vec4 r = paneRect(i);
        if (!inside(pixel, r)) continue;
        color = mix(paper(), color, smoothstep(0.0, p_fade, p));
        float energy = fading(p);
        return drawRim(color, across(pixel, r), rimDistance(pixel, r), p_rimWidth, p_rim * energy, p_reach, p_glow * energy);
    }
    return color;
}

// Scripts' pulses, in the palette colour that the script chose. A shake
// reads the input again, so pulses come before the context lines, which then
// still mark a shaking pane.
vec4 pulses(vec2 pixel, vec4 color) {
    for (int i = 0; i < paneCount(); ++i) {
        float p = panePulse(i);
        if (p >= 1.0 || !paneInCurrentTab(i)) continue;
        vec4 r = paneRect(i);
        if (!inside(pixel, r)) continue;
        int effect = panePulseEffect(i);
        vec3 tint = panePulseColor(i);
        float energy = fading(p);
        if (effect == 1) {
            // A ring of light that sweeps out from the pane's centre.
            float radius = length(r.zw) * 0.55 * (1.0 - pow(1.0 - p, 3.0));
            float band = max(min(r.z, r.w) * 0.05, 2.0);
            float k = (length(pixel - r.xy - r.zw * 0.5) - radius) / band;
            color = addLight(color, tint * exp(-k * k) * (1.0 - p) * 0.8 * max(color.a, 0.5));
        } else if (effect == 2) {
            // The rim in the pulse's colour, and a faint light over the pane.
            color = addLight(color, tint * energy * 0.08 * max(color.a, 0.5));
            color = drawRim(color, tint, rimDistance(pixel, r), p_rimWidth, energy, p_reach, energy);
        } else if (effect == 3) {
            // A sideways shake that settles.
            float shift = sin(p * 42.0) * 9.0 * energy;
            color = terminalTexel(clampToRect(pixel - vec2(shift, 0.0), r) / iResolution);
        }
    }
    return color;
}

// The lines of panes away from here.
vec4 contextMarks(vec2 pixel, vec4 color) {
    for (int i = 0; i < paneCount(); ++i) {
        if (!paneInCurrentTab(i)) continue;
        int context = paneContext(i);
        float p = paneContextChange(i);
        if (context == 0 && p >= 1.0) continue;
        vec4 r = paneRect(i);
        if (!inside(pixel, r)) continue;
        bool away = context != 0;
        int previous = panePreviousContext(i);
        float q = smoothstep(0.0, 1.0, p);
        // A pane that returns here fades its line out in the colour it had.
        // Between two contexts, the line stays and changes colour.
        vec3 tint = away ? spectrumContext(context) : spectrumContext(previous);
        if (away && previous != 0) tint = mix(spectrumContext(previous), tint, q);
        float present = away ? 1.0 : 1.0 - q;
        // Bright at the change, then the line settles and the glow goes.
        float energy = fading(p);
        float line = p_mark * mix(0.7, 1.0, energy) * present;
        color = drawRim(color, tint, rimDistance(pixel, r), p_rimWidth, line, p_reach, 0.5 * p_glow * energy * min(p_mark * 2.0, 1.0));
    }
    return color;
}

vec4 pTerminal(vec2 uv) {
    vec2 pixel = terminalPixel(uv);
    vec4 color = opening(pixel, closing(pixel, terminalTexel(uv)));
    return contextMarks(pixel, pulses(pixel, color));
}
