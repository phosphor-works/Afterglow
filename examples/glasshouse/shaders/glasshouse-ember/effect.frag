// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Glasshouse ember, for the cursor layer. The cursor glows like the
// Glasshouse lamp: a hot core and a soft halo, as light added. When it jumps,
// sparks fly from the path it took, each a short streak along its flight,
// slowing and falling. The light world inks the sparks in leaf and sepia and
// gives the cursor a honey core.

#include "glasshouse.glsl"

float ghStreak(vec2 d, vec2 dir, float len, float wid)
{
    float along = dot(d, dir);
    float across = dot(d, vec2(-dir.y, dir.x));
    return exp(-ghSq(across / wid)) * exp(-ghSq(max(abs(along) - len * 0.5, 0.0) / wid));
}

vec4 pTerminal(vec2 uv)
{
    GhWorld w = ghWorld();
    vec2 pixel = terminalPixel(uv);
    vec4 cur = terminalTexel(uv);
    vec4 c = cur;

    vec2 halfSize = uCursorRect.zw * 0.5;
    vec2 centre = cursorCenter();
    vec2 q = (pixel - centre) / max(halfSize, vec2(1.0));
    bool shown = cursorVisible() && paneFocused();

    if (shown && cur.a > 0.0) {
        // The core runs hot in the middle.
        vec3 core = w.ink ? mix(cur.rgb / cur.a, vec3(1.0, 0.84, 0.42), 0.7 * exp(-6.0 * dot(q * 0.5, q * 0.5)))
                          : mix(cur.rgb / cur.a, w.hot, 0.5 * exp(-2.5 * dot(q, q)));
        c = vec4(core * cur.a, cur.a);
    }
    if (shown && p_glow > 0.0) {
        // The halo, breathing slowly.
        vec2 dd = (pixel - centre) / (uCellSize.y * 1.6);
        float halo = exp(-dot(dd, dd) * 4.0) * (1.0 - cur.a) * (0.85 + 0.15 * sin(timeAngle(2.4))) * p_glow;
        c = w.ink ? c + vec4(w.leaf, 1.0) * halo * 0.25 : c + vec4(mix(w.lamp, w.hot, 0.3) * halo * 0.6, 0.0);
    }

    // Sparks from a jump, only from a cursor that shows.
    float age = cursorAge();
    vec2 from = previousCursorCenter();
    float jump = distance(from, centre);
    if (shown && p_sparks > 0.0 && age < p_sparkLife * 1.5 && jump > uCellSize.x * 1.5) {
        // Each jump between two places throws its own sparks.
        float seed = ghEventSeed((from + centre * 0.37) / max(uCellSize, vec2(1.0)));
        vec2 path = (centre - from) / max(jump, 1.0);
        vec2 side = vec2(-path.y, path.x);
        for (int i = 0; i < 24; ++i) {
            float fi = float(i);
            vec3 h = vec3(ghHash(vec2(fi, seed)), ghHash(vec2(fi + 3.3, seed)), ghHash(vec2(fi + 7.1, seed)));
            float life = p_sparkLife * (0.6 + 0.8 * h.z);
            if (age > life) {
                continue;
            }
            float k = age / life;
            vec2 origin = mix(from, centre, h.x);
            // Thrown sideways off the path and a little forwards, then drag and
            // gravity, which pulls down the screen (+y).
            vec2 v0 = (side * (h.y - 0.5) * 2.0 + path * 0.35) * uCellSize.y * 9.0;
            float drag = 0.25;
            vec2 pos = origin + v0 * drag * (1.0 - exp(-age / drag)) + vec2(0.0, 1.0) * uCellSize.y * 14.0 * age * age;
            vec2 vel = v0 * exp(-age / drag) + vec2(0.0, 1.0) * uCellSize.y * 28.0 * age;
            float speed = length(vel);
            vec2 dir = speed > 1e-3 ? vel / speed : path;
            float len = 2.0 + speed * 0.035;
            float fade = pow(1.0 - k, 1.6) * (0.75 + 0.25 * sin(age * 40.0 + 40.0 * h.x));
            float s = ghStreak(pixel - pos, dir, len, 0.8) * fade * p_sparks;
            vec3 tint = mix(w.hot, w.keyline, k);
            c = w.ink ? c + vec4(mix(w.leaf, w.sepia, k * 0.6), 1.0) * s * 0.85 : c + vec4(tint * s * 1.7, 0.0);
        }
    }
    return clamp(c, 0.0, 1.0);
}
