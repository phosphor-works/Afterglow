// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Glasshouse mullion, for the pane chain. Under Glasshouse the window's
// frame is Vitrail's, so only the edges a pane shares with another split get
// lead: each pane draws its half of the mullion, and the two halves meet over
// the divider. When focus arrives, gilding runs along the pane's mullions
// behind a hot tip and cools to the keyline. Signal 3 (the bell) sends a
// shock ring through the glass, bending it as it passes.
//
// Only event ages move it, so the window stops rendering settleSeconds after
// the last one. 12 s covers the slowest gilding: at gildTime 3 the tip takes
// 5 s, and on a small pane the gilt behind it cools for 6 s more.

#include <terminal_panes.glsl>
#include "glasshouse.glsl"

vec4 pTerminal(vec2 uv)
{
    GhWorld w = ghWorld();
    vec2 res = iResolution;
    vec2 pixel = terminalPixel(uv);

    // The bell: a ring rolling out from the cursor, refracting what it crosses.
    vec2 shockAt = cursorCenter();
    float ringAge = ghAge(3);
    vec2 bend = vec2(0.0);
    float ringGlow = 0.0;
    if (ringAge < 0.9 && p_shock > 0.0) {
        vec2 d = pixel - shockAt;
        float r = length(d);
        float ringR = ringAge * length(res) * 0.9;
        float x = (r - ringR) / (uCellSize.y * 1.2);
        float fade = (1.0 - ringAge / 0.9) * p_shock;
        float g = exp(-x * x);
        bend = (r > 0.0 ? d / r : vec2(0.0)) * (-2.33 * x * g) * 3.0 * fade;
        ringGlow = g * fade;
    }
    vec4 c = terminalTexel(terminalUv(pixel + bend));
    if (ringGlow > 0.0 && !w.ink) {
        // A touch of colour fringing in the bent glass.
        c.r = terminalTexel(terminalUv(pixel + bend * 1.15)).r;
        c.b = terminalTexel(terminalUv(pixel + bend * 0.85)).b;
    }
    c = ghLightUp(w, c, mix(w.hot, w.jewel1, 0.35), ringGlow * (w.ink ? 0.12 : 0.3));

    // The shared edges: those away from the window's own edge.
    float tol = 2.0;
    bool left = uTerminalRect.x > tol;
    bool right = uTerminalRect.x + uTerminalRect.z < uWindowSize.x - tol;
    // The top edge is shared when another pane of the tab starts higher up,
    // whatever the tab bar's height.
    float firstRow = uTerminalRect.y;
    for (int i = 0; i < paneCount(); ++i) {
        if (paneInCurrentTab(i)) firstRow = min(firstRow, paneRect(i).y);
    }
    bool top = uTerminalRect.y > firstRow + tol;
    bool bottom = uTerminalRect.y + uTerminalRect.w < uWindowSize.y - tol;
    vec2 p = pixel;
    float edge = 1e4;
    float along = 0.0; // how far along the pane's mullions, in pixels
    if (left && p.x < edge) {
        edge = p.x;
        along = p.y;
    }
    if (right && res.x - p.x < edge) {
        edge = res.x - p.x;
        along = res.y + p.y;
    }
    if (top && p.y < edge) {
        edge = p.y;
        along = 2.0 * res.y + p.x;
    }
    if (bottom && res.y - p.y < edge) {
        edge = res.y - p.y;
        along = 2.0 * res.y + res.x + p.x;
    }

    if (edge < 1e3) {
        float came = 1.0 - smoothstep(p_came - 0.5, p_came + 0.5, edge);
        // Half the moulding; the divider is its centre line.
        vec3 moulding = ghMoulding(w, 0.5 + 0.5 * edge / p_came);
        c = mix(c, vec4(moulding, 1.0), came);
        vec3 gold = w.ink ? w.leaf : w.keyline;
        float keyline = exp(-ghSq((edge - p_came - 0.6) / 0.7));
        if (paneFocused()) {
            // Gilding behind a hot tip, which runs the length of the mullions.
            float total = 2.0 * (res.x + res.y);
            float run = focusAge() / p_gildTime * total * 0.6;
            float age = (run - along) / (uCellSize.y * 4.0);
            float gilt = step(0.0, age) * (0.45 + 0.8 * exp(-max(age, 0.0) / 3.0));
            float tip = exp(-ghSq((run - along) / (uCellSize.y * 1.5)));
            c = ghLightUp(w, c, gold, gilt * exp(-ghSq((edge - p_came) / 1.1)) * (w.ink ? 0.5 : 0.8));
            c = ghLightUp(w, c, mix(w.hot, vec3(1.0), 0.4), tip * exp(-edge / (p_came * 2.5)) * 1.4);
        } else {
            c = ghLightUp(w, c, gold, keyline * 0.15);
        }
    }
    // Premultiplied: light added over a translucent window stops at its alpha.
    c.a = clamp(c.a, 0.0, 1.0);
    return vec4(clamp(c.rgb, vec3(0.0), vec3(c.a)), c.a);
}
