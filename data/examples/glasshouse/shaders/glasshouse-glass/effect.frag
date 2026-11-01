// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Glasshouse glass, for the background layer: quarry glazing. Diamonds of
// stained glass in the world's jewels, each stained in proportion to the
// light behind it, joined by lit lead came with a keyline beside it, under a
// lamp hanging above the pane. The text is its own layer and stays sharp.
//
// Signals (glasshouse.luau):
//   0 strike  cracks run out from the prompt with a hot tip, the glass calls
//             in the alert colour, then it mends
//   1 glaze   light sweeps along the lead
//   4 status  1 working (the lamp breathes), 2 blocked, 3 error (it calls)
// The `heat` parameter warms the lamp and keylines while a command runs. The
// script moves it with a transition, which starts from the heat on screen,
// so a short command cannot make it jump.

#include "glasshouse.glsl"

vec4 pTerminal(vec2 uv)
{
    vec4 layer = terminalTexel(uv);
    // Pixel position from the top left, as the lattice is laid.
    vec2 p = terminalPixel(uv);
    float cy = max(uCellSize.y, 1.0) * p_quarry;
    vec4 q = ghQuarry(p, vec2(cy * 0.62, cy));
    // The came's antialiasing width, taken before any pixel returns early:
    // derivatives are undefined once part of a quad has left.
    float ex = q.z;
    float fw = max(fwidth(ex), 1e-3);
    if (layer.a <= 0.0) {
        return layer;
    }
    GhWorld w = ghWorld();
    vec3 straight = layer.rgb / layer.a;
    // Cells with a background of their own (selections, status lines) keep it.
    float plain = 1.0 - smoothstep(0.03, 0.12, distance(straight, uBackgroundColor.rgb));

    float h = ghHash(q.xy);
    vec3 jewel = ghJewel(w, fract(h * 7.31 + q.x * 0.37));

    // The lamp hangs above the pane and drifts a little; heat and status move it.
    float heat = clamp(p_heat, 0.0, 1.0);
    int status = int(signalValue(4) + 0.5);
    float breathe = status == 1 ? 0.5 + 0.5 * sin(timeAngle(2.2)) : 0.0;
    vec2 lamp = vec2(iResolution.x * (0.5 + 0.08 * sin(timeAngle(0.05))), -0.15 * iResolution.y);
    float radius = length(iResolution.xy) * 0.75;
    float fall = exp(-dot(p - lamp, p - lamp) / (radius * radius));
    float power = p_lamp * (1.0 + 0.6 * heat + 0.3 * breathe);
    float lit = mix(w.unlit, 1.0, 0.45) + power * fall * 0.5;

    // Stained by the light behind it, as the Glasshouse windows are.
    float lum = ghLuma(straight);
    vec3 glass = w.ink ? jewel * (0.6 + 0.45 * lum) : jewel * (0.45 + 1.7 * lum) * 1.15 + straight * 0.2;
    glass *= lit;
    float grain = 1.0 - p_grain + 2.0 * p_grain * ghNoise(p * 0.7 + h * 50.0);
    vec3 col = mix(straight, glass * grain, p_stain * plain * (w.ink ? 0.45 : 1.0));
    if (!w.ink) {
        vec3 pool = mix(w.lamp, w.hot, heat * 0.6) / 1.6;
        col += pool * fall * power * 0.08 * plain;
    }
    // A sheen across each quarry, at its own place on the diagonal.
    float sheen = exp(-ghSq((q.w - fract(h * 3.7) * 2.0) / 0.22)) * step(0.72, fract(h * 13.1)) * 0.06;
    col += mix(w.hot, vec3(1.0), 0.35) * sheen * plain * (w.ink ? 0.5 : 1.0);

    vec4 c = vec4(col * layer.a, layer.a);

    // Strike: the pane cracks from the prompt like struck glass, rays and
    // rings spreading out. The cracks glow hot, cool to scars, then mend.
    float strikeAge = ghAge(0);
    if (strikeAge < 4.5) {
        // From the start of the line the failure returned to. The strike comes
        // before the prompt is drawn; the line start stays put while the next
        // command is typed, where the cursor would drag the cracks along.
        vec2 origin = vec2(0.0, uCursorRect.y + uCursorRect.w * 0.5);
        vec2 d = p - origin;
        float r = length(d);
        float th = atan(d.y, d.x);
        float seed = floor(ghSetTime(strikeAge) * 7.0);
        float maxR = length(iResolution.xy);
        float front = (1.0 - exp(-strikeAge * 6.0)) * maxR * 0.55;
        float best = 1e4;
        float sector = 0.0;
        float lastDa = 1e4;
        for (int k = 0; k < 11; ++k) {
            float fk = float(k);
            float base = (fk + 0.8 * (ghHash(vec2(fk, seed)) - 0.5)) * 6.2831853 / 11.0 + seed;
            float a = base + 0.22 * (ghNoise(vec2(r * 0.04 + fk * 7.0, seed)) - 0.5) + 0.07 * (ghNoise(vec2(r * 0.23 + fk * 3.0, seed)) - 0.5);
            float da = th - a;
            da -= 6.2831853 * floor((da + 3.14159265) / 6.2831853);
            float len = maxR * (0.15 + 0.4 * ghHash(vec2(fk + 9.0, seed)));
            if (abs(da) < 1.5707963 && r < min(len, front)) {
                best = min(best, r * abs(sin(da)));
            }
            if (da >= 0.0 && da < lastDa) {
                lastDa = da;
                sector = fk;
            }
        }
        float ringD = 1e4;
        float ring = 0.0;
        for (int j = 0; j < 2; ++j) {
            float fj = float(j);
            float R = cy * (0.9 + 1.4 * fj + 0.7 * ghHash(vec2(sector * 3.0 + fj * 17.0, seed)));
            R += 3.0 * (ghNoise(vec2(th * 9.0 + sector + fj * 5.0, seed)) - 0.5);
            if (ghHash(vec2(sector * 5.0 + fj * 11.0, seed)) < (j == 0 ? 0.7 : 0.4) && R < front) {
                ringD = min(ringD, abs(r - R));
            }
            ring += step(R, r);
        }
        float dist = min(best, ringD);
        float inside = (1.0 - smoothstep(front - 2.0, front + 2.0, r));
        float mend = 1.0 - smoothstep(2.2, 4.5, strikeAge);
        float heatOf = exp(-strikeAge / 0.8);
        // Each shard sits a little askew.
        float piece = sector * 3.0 + ring;
        float tilt = (ghHash(vec2(piece + 53.0, seed)) - 0.5) * 0.24 * inside * mend;
        c.rgb *= 1.0 + tilt * plain;
        float line = (1.0 - smoothstep(0.6, 1.8, dist)) * inside * mend;
        float halo = exp(-dist * dist / 14.0) * inside * mend;
        float near = 0.55 + 0.45 * exp(-r / max(cy * 4.0, 1.0));
        vec3 rent = w.ink ? mix(w.keyline, w.sepia, 0.3) : mix(w.hot, vec3(1.0), 0.55);
        c = ghLightUp(w, c, rent, (line * 1.4 + halo * 0.45) * heatOf * near);
        // Cooled, the cracks stay as scars until the glass mends.
        vec3 scar = w.ink ? w.sepia : w.metalSky;
        c.rgb = mix(c.rgb, scar * c.a, line * (1.0 - heatOf) * 0.85);
        c.rgb *= 1.0 - 0.4 * halo * (1.0 - heatOf);
        // The alert calls through the glass and fades.
        float call = exp(-r / (cy * 3.0)) * (1.0 - smoothstep(0.0, 2.5, strikeAge));
        c.rgb = mix(c.rgb, w.alert * (w.ink ? 0.95 : 0.5) * c.a, call * (w.ink ? 0.2 : 0.35) * plain);
    }
    if (status >= 2) {
        float call = (status == 3 ? 0.28 : 0.16) * (0.6 + 0.4 * sin(timeAngle(3.0)));
        c.rgb = mix(c.rgb, w.alert * (w.ink ? 0.95 : 0.5) * c.a, call * fall * plain);
    }

    // The came: lit moulding, opaque even on a translucent window, with the
    // keyline just outside it. Glaze sends light along it.
    float lead = (1.0 - smoothstep(p_came - fw, p_came + fw, ex)) * mix(0.35, 1.0, plain);
    vec3 moulding = ghMoulding(w, 0.5 + 0.5 * ex / p_came);
    float glazeAge = ghAge(1);
    float sweep = glazeAge < 1.8 ? glazeAge / 1.4 : -1.0;
    float aspect = iResolution.x / iResolution.y;
    float sd = (uv.x * aspect + (1.0 - (p.y / iResolution.y)) * 0.6) - sweep * (aspect + 0.6);
    float catchLight = exp(-sd * sd * 180.0) * (sweep >= 0.0 ? 1.0 : 0.0);
    moulding += w.metalHi * catchLight * 1.2;
    c = mix(c, vec4(moulding, 1.0), lead);
    float key = exp(-ghSq((ex - p_came - 0.6) / 0.7)) * plain;
    vec3 keyTint = mix(w.keyline, w.hot, heat * 0.5);
    c.rgb += keyTint * key * (0.4 + 0.3 * heat + 0.8 * catchLight) * (w.ink ? 0.6 : 1.0) * c.a;

    // Premultiplied: light added over a translucent window stops at its alpha.
    c.a = clamp(c.a, 0.0, 1.0);
    return vec4(clamp(c.rgb, vec3(0.0), vec3(c.a)), c.a);
}
