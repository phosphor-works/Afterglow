// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Flat pane effects, for the pane effects packs that Afterglow installs
// (Phosphor Panes, Plain Panes) and for packs that want the same motion.
// Nothing turns or bends: a position moves on the window's own split
// animation, and this file changes only light.
//   open     the pane's picture fades in from its background
//   close    the pane's last frame fades out over what took its place
//   context  a pane whose shell moves to another host, user or container
//            (paneContext()) keeps a thin line at its edge. The line shows
//            at once, and fades out when the pane returns
//   pulses   afterglow.pulse(): a ring, a flash at the edge or a shake
// The input holds every open pane where it is; a closed pane's last frame
// comes from closingPaneTexel() at closingPaneRect(). Every progress is
// linear, and this file eases it. At progress 1 nothing changes.
//
// A pack declares the parameters fade, rimWidth and mark, defines the macros
// that it wants to change, includes this file, and returns flatPanes(uv)
// from its pTerminal(). Afterglow looks for pTerminal() in the pack's own
// file, so this file cannot define it. The macros:
//   PANE_CONTEXT_TINT(context)  the line's colour for a paneContext() value
//                               (required)
//   PANE_EDGE_TINT(pixel, r)    the colour of the rim of a pane that opens or
//                               closes, at a window pixel of its rectangle r
//   PANE_RIM                    that rim's strength, 0 to 1 (default 0: none)
//   PANE_GLOW, PANE_REACH       the strength of the halo inside a rim, and how
//                               far it reaches in pixels (default 0: none)
//   PANE_HALO                   1 when a flash pulse has a halo (default 0)
//   PANE_SETTLED                the line's strength once a context change has
//                               settled, against 1 at the change (default 1)
//
// This file is part of each pack that includes it. Read no clock and no event
// age here: the pack would then render continuously.

#ifndef AFTERGLOW_FLAT_PANES_GLSL
#define AFTERGLOW_FLAT_PANES_GLSL

#include <terminal_panes.glsl>
#include <rim.glsl>

#ifndef PANE_EDGE_TINT
#define PANE_EDGE_TINT(pixel, r) vec3(0.0)
#endif
#ifndef PANE_RIM
#define PANE_RIM 0.0
#endif
#ifndef PANE_GLOW
#define PANE_GLOW 0.0
#endif
#ifndef PANE_REACH
#define PANE_REACH 1.0
#endif
#ifndef PANE_HALO
#define PANE_HALO 0.0
#endif
#ifndef PANE_SETTLED
#define PANE_SETTLED 1.0
#endif

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
    return drawRim(color, PANE_EDGE_TINT(pixel, r), rimDistance(pixel, r), p_rimWidth, PANE_RIM * energy, PANE_REACH, PANE_GLOW * energy);
}

vec4 opening(vec2 pixel, vec4 color) {
    for (int i = 0; i < paneCount(); ++i) {
        float p = paneOpening(i);
        if (p >= 1.0 || paneOpenEdge(i) == 0 || !paneInCurrentTab(i)) continue;
        vec4 r = paneRect(i);
        if (!inside(pixel, r)) continue;
        color = mix(paper(), color, smoothstep(0.0, p_fade, p));
        float energy = fading(p);
        return drawRim(color, PANE_EDGE_TINT(pixel, r), rimDistance(pixel, r), p_rimWidth, PANE_RIM * energy, PANE_REACH, PANE_GLOW * energy);
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
            color = addLight(color, tint * energy * mix(0.25, 0.08, PANE_HALO) * max(color.a, 0.5));
            color = drawRim(color, tint, rimDistance(pixel, r), p_rimWidth, energy, PANE_REACH, PANE_HALO * energy);
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
        vec3 tint = away ? PANE_CONTEXT_TINT(context) : PANE_CONTEXT_TINT(previous);
        if (away && previous != 0) tint = mix(PANE_CONTEXT_TINT(previous), tint, q);
        float present = away ? 1.0 : 1.0 - q;
        // Bright at the change, then the line settles and the glow goes.
        float energy = fading(p);
        float line = p_mark * mix(PANE_SETTLED, 1.0, energy) * present;
        color = drawRim(color, tint, rimDistance(pixel, r), p_rimWidth, line, PANE_REACH, 0.5 * PANE_GLOW * energy * min(p_mark * 2.0, 1.0));
    }
    return color;
}

vec4 flatPanes(vec2 uv) {
    vec2 pixel = terminalPixel(uv);
    vec4 color = opening(pixel, closing(pixel, terminalTexel(uv)));
    return contextMarks(pixel, pulses(pixel, color));
}

#endif // AFTERGLOW_FLAT_PANES_GLSL
