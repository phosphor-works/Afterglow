// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A flat tab transition, for the tab transition packs that Afterglow
// installs (Phosphor Tabs, Plain Tabs) and for packs that want the same
// motion. Both tabs stay flat. The new tab is the pass's input and the tab
// being left comes from previousTabTexel(); both keep running. Two styles:
//   0 slide  the new tab slides in over the tab being left, which trails
//            behind it and dims
//   1 fade   the tabs blend, with no motion
// The window shows the tab being left exactly at progress 0 and the new tab
// at progress 1. Animation.tabCurve eases the progress, so this file times
// nothing itself.
//
// A pack declares the parameters style, parallax and dim, includes this
// file, and returns flatTabs(uv) from its pTerminal(). Afterglow looks for
// pTerminal() in the pack's own file, so this file cannot define it. For a
// line down the edge between the tabs, the pack
// first defines TAB_SEAM_TINT(t), the line's colour from the top of the
// window (0) to the bottom (1), and declares the parameters seam, seamWidth,
// glow and reach.
//
// This file is part of each pack that includes it. Read no clock and no event
// age here: the pack would then render continuously.

#ifndef AFTERGLOW_FLAT_TABS_GLSL
#define AFTERGLOW_FLAT_TABS_GLSL

#include <terminal_panes.glsl>
#include <rim.glsl>

vec4 slideTabs(vec2 uv, float progress, float dir) {
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
#ifdef TAB_SEAM_TINT
    // The edge: bright at once, and gone as the new tab arrives. It is not
    // there at progress 0 and 1, where the window shows one tab alone.
    float energy = smoothstep(0.0, 0.04, p) * (1.0 - smoothstep(0.55, 1.0, p));
    float away = abs(pixel.x - seam);
    float line = (1.0 - smoothstep(p_seamWidth * 0.5 - 0.5, p_seamWidth * 0.5 + 0.5, away)) * p_seam;
    // The halo falls on the tab being left: the new tab is the surface above it.
    float halo = onNew ? 0.0 : exp(-away / p_reach) * p_glow;
    vec3 tint = TAB_SEAM_TINT(uv.y);
    color = addLight(color, tint * halo * energy * max(color.a, 0.5));
    color = coverWith(color, tint, line * energy);
#endif
    return color;
}

vec4 flatTabs(vec2 uv) {
    if (!tabTransitionActive()) return terminalTexel(uv);
    float progress = tabTransitionProgress();
    if (int(p_style + 0.5) == 1) return mix(previousTabTexel(uv), terminalTexel(uv), clamp(progress, 0.0, 1.0));
    return slideTabs(uv, progress, tabTransitionDirection());
}

#endif // AFTERGLOW_FLAT_TABS_GLSL
