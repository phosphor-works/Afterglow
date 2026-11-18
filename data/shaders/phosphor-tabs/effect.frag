// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Phosphor Tabs: the tab transition, in the Phosphor design language. Both
// tabs stay flat. The new tab is this pass's input and the tab being left
// comes from previousTabTexel(); both keep running. Two styles:
//   0 slide  the new tab slides in over the tab being left, which trails
//            behind it and dims; the edge between them carries the spectrum
//   1 fade   the tabs blend, with no motion
// The window shows the tab being left exactly at progress 0 and the new tab
// at progress 1. Animation.tabCurve eases the progress, so the pack times
// nothing itself and reads no clock.

#include <terminal_panes.glsl>
#include <phosphor_spectrum.glsl>

vec4 slide(vec2 uv, float progress, float dir) {
    vec2 res = iResolution;
    vec2 pixel = terminalPixel(uv);
    float p = clamp(progress, 0.0, 1.0);
    // How far the new tab still has to go, in window pixels. A spring can
    // carry the progress past 1: the tab then stays where it belongs.
    float travel = max(1.0 - progress, 0.0) * res.x;
    float seam = dir > 0.0 ? travel : res.x - travel;
    bool onNew = dir > 0.0 ? pixel.x >= seam : pixel.x < seam;

    vec4 color;
    if (onNew) {
        float x = clamp(pixel.x - dir * travel, 0.5, res.x - 0.5);
        color = terminalTexel(vec2(x / res.x, uv.y));
    } else {
        // The tab being left moves a part of the way, so it falls behind.
        float x = clamp(pixel.x + dir * p * res.x * p_parallax, 0.5, res.x - 0.5);
        color = previousTabTexel(vec2(x / res.x, uv.y));
        color.rgb *= 1.0 - p_dim * p;
    }

    // The edge: bright at once, and gone as the new tab arrives. It is not
    // there at progress 0 and 1, where the window shows one tab alone.
    float energy = smoothstep(0.0, 0.04, p) * (1.0 - smoothstep(0.55, 1.0, p));
    float away = abs(pixel.x - seam);
    float line = (1.0 - smoothstep(p_seamWidth * 0.5 - 0.5, p_seamWidth * 0.5 + 0.5, away)) * p_seam;
    // The halo falls on the tab being left: the new tab is the surface above it.
    float halo = onNew ? 0.0 : exp(-away / p_reach) * p_glow;
    vec3 tint = spectrum(uv.y);
    color = addLight(color, tint * halo * energy * max(color.a, 0.5));
    return coverWith(color, tint, line * energy);
}

vec4 pTerminal(vec2 uv) {
    if (!tabTransitionActive()) return terminalTexel(uv);
    float progress = tabTransitionProgress();
    if (int(p_style + 0.5) == 1) {
        float p = clamp(progress, 0.0, 1.0);
        return mix(previousTabTexel(uv), terminalTexel(uv), p);
    }
    return slide(uv, progress, tabTransitionDirection());
}
