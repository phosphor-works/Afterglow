// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Glasshouse mullion, for the pane chain. Under Glasshouse the window's
// frame is Vitrail's, so only the edges a pane shares with another split get
// lead: each pane draws its half of the mullion, and the two halves meet over
// the divider. When focus arrives, gilding runs along the pane's mullions
// behind a hot tip and cools to the keyline. iSignal[3] (the bell) sends a
// shock ring through the glass, bending it as it passes.

#include "glasshouse.glsl"

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    GhWorld w = ghWorld();
    vec2 res = iResolution.xy;

    // The bell: a ring rolling out from the cursor, refracting what it crosses.
    vec2 shockAt = iCurrentCursor.xy + vec2(iCurrentCursor.z * 0.5, -iCurrentCursor.w * 0.5);
    float ringAge = ghAge(3);
    vec2 bend = vec2(0.0);
    float ringGlow = 0.0;
    if (ringAge < 0.9 && p_shock > 0.0) {
        vec2 d = fragCoord - shockAt;
        float r = length(d);
        float ringR = ringAge * length(res) * 0.9;
        float x = (r - ringR) / (iCellSize.y * 1.2);
        float fade = (1.0 - ringAge / 0.9) * p_shock;
        float g = exp(-x * x);
        bend = (r > 0.0 ? d / r : vec2(0.0)) * (-2.33 * x * g) * 3.0 * fade;
        ringGlow = g * fade;
    }
    vec4 c = texture(iChannel0, (fragCoord + bend) / res);
    if (ringGlow > 0.0 && !w.ink) {
        // A touch of colour fringing in the bent glass.
        c.r = texture(iChannel0, (fragCoord + bend * 1.15) / res).r;
        c.b = texture(iChannel0, (fragCoord + bend * 0.85) / res).b;
    }
    c = ghLightUp(w, c, mix(w.hot, w.jewel1, 0.35), ringGlow * (w.ink ? 0.12 : 0.3));

    // The shared edges: those away from the window's own edge.
    float tol = 2.0;
    bool left = iRegion.x > tol;
    bool right = iRegion.x + iRegion.z < iFramebufferSize.x - tol;
    bool top = iRegion.y > iCellSize.y * 3.0; // below another pane, not the tab bar
    bool bottom = iRegion.y + iRegion.w < iFramebufferSize.y - tol;
    vec2 p = vec2(fragCoord.x, res.y - fragCoord.y); // from the top left
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
        if (iFocus != 0) {
            // Gilding behind a hot tip, which runs the length of the mullions.
            float total = 2.0 * (res.x + res.y);
            float run = (iTime - iTimeFocus) / p_gildTime * total * 0.6;
            float age = (run - along) / (iCellSize.y * 4.0);
            float gilt = step(0.0, age) * (0.45 + 0.8 * exp(-max(age, 0.0) / 3.0));
            float tip = exp(-ghSq((run - along) / (iCellSize.y * 1.5)));
            c = ghLightUp(w, c, gold, gilt * exp(-ghSq((edge - p_came) / 1.1)) * (w.ink ? 0.5 : 0.8));
            c = ghLightUp(w, c, mix(w.hot, vec3(1.0), 0.4), tip * exp(-edge / (p_came * 2.5)) * 1.4);
        } else {
            c = ghLightUp(w, c, gold, keyline * 0.15);
        }
    }
    fragColor = clamp(c, 0.0, 1.0);
}
