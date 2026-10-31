// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Glasshouse look, shared by the glasshouse packs. The colours are the
// Glasshouse theme's two worlds (Dsaquel/dots, themes/glasshouse). The dark
// world lights effects additively; the light world inks them on in sepia and
// gold leaf. The world follows the terminal's background.

struct GhWorld
{
    bool ink;
    vec3 metalBase, metalHi, metalLow, metalSky;
    vec3 jewel0, jewel1, jewel2;
    vec3 hot, keyline, lamp, alert, sepia, leaf;
    float unlit;
};

float ghLuma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}

GhWorld ghWorld()
{
    GhWorld w;
    w.ink = ghLuma(uBackgroundColor.rgb) > 0.5;
    if (w.ink) {
        w.metalBase = vec3(0.792, 0.639, 0.416); // #caa36a
        w.metalHi = vec3(1.0, 0.969, 0.886); // #fff7e2
        w.metalLow = vec3(0.290, 0.184, 0.110); // #4a2f1c
        w.metalSky = vec3(0.984, 0.890, 0.690); // #fbe3b0
        w.jewel0 = vec3(0.910, 0.459, 0.604); // #e8759a
        w.jewel1 = vec3(0.953, 0.757, 0.310); // #f3c14f
        w.jewel2 = vec3(0.373, 0.827, 0.902); // #5fd3e6
        w.hot = vec3(1.0, 0.941, 0.784); // #fff0c8
        w.keyline = vec3(0.812, 0.608, 0.247); // leaf #cf9b3f
        w.lamp = vec3(1.0, 0.831, 0.541); // #ffd48a
        w.alert = vec3(0.812, 0.231, 0.290); // #cf3b4a
        w.sepia = vec3(0.290, 0.184, 0.110); // #4a2f1c
        w.leaf = vec3(0.812, 0.608, 0.247); // #cf9b3f
        w.unlit = 0.72;
    } else {
        w.metalBase = vec3(0.227, 0.224, 0.314); // #3a3950
        w.metalHi = vec3(0.839, 0.827, 0.949); // #d6d3f2
        w.metalLow = vec3(0.024, 0.027, 0.055); // #06070e
        w.metalSky = vec3(0.490, 0.533, 0.729); // #7d88ba
        w.jewel0 = vec3(0.208, 0.345, 0.722); // #3558b8
        w.jewel1 = vec3(0.416, 0.310, 0.761); // #6a4fc2
        w.jewel2 = vec3(0.247, 0.561, 0.525); // #3f8f86
        w.hot = vec3(1.0, 0.851, 0.627); // #ffd9a0
        w.keyline = vec3(0.949, 0.702, 0.369); // gold #f2b35e
        w.lamp = vec3(0.949, 0.702, 0.369); // #f2b35e
        w.alert = vec3(0.878, 0.400, 0.541); // #e0668a
        w.sepia = vec3(0.024, 0.027, 0.055); // #06070e
        w.leaf = vec3(0.949, 0.702, 0.369); // #f2b35e
        w.unlit = 0.56;
    }
    return w;
}

float ghSq(float x)
{
    return x * x;
}

float ghHash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float ghNoise(vec2 x)
{
    vec2 i = floor(x);
    vec2 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(ghHash(i), ghHash(i + vec2(1.0, 0.0)), f.x), mix(ghHash(i + vec2(0.0, 1.0)), ghHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

vec3 ghJewel(GhWorld w, float h)
{
    return h < 0.34 ? w.jewel0 : (h < 0.67 ? w.jewel1 : w.jewel2);
}

// Quarry glazing: diamonds of a diagonal lattice with diagonals cell.x wide
// and cell.y tall. xy: the diamond's lattice cell (its id), z: the distance
// from p to the diamond's edge in pixels, w: position along the lattice
// diagonal, for sheens.
vec4 ghQuarry(vec2 p, vec2 cell)
{
    vec2 lat = vec2(p.x / cell.x + p.y / cell.y, p.x / cell.x - p.y / cell.y);
    vec2 gi = floor(lat);
    vec2 gf = lat - gi;
    float edge = min(min(gf.x, 1.0 - gf.x), min(gf.y, 1.0 - gf.y)) * min(cell.x, cell.y) * 0.7071;
    return vec4(gi, edge, gf.x + gf.y);
}

// Lead came seen across its width (0 to 1): a rounded, lit moulding.
vec3 ghMoulding(GhWorld w, float across)
{
    float h = sin(3.14159265 * clamp(across, 0.0, 1.0));
    vec3 c = mix(w.metalLow, w.metalBase * 1.1, h) + w.metalHi * pow(h, 12.0) * 0.2;
    return w.ink ? mix(c, w.sepia, 0.55) : c;
}

// Effects: light added in the dark world, ink laid on in the light world.
// col is premultiplied.
vec4 ghLightUp(GhWorld w, vec4 col, vec3 tint, float k)
{
    k = clamp(k, 0.0, 1.5);
    return w.ink ? mix(col, vec4(tint, 1.0), clamp(k, 0.0, 1.0)) : col + vec4(tint * k, 0.0);
}

float ghAge(int channel)
{
    return signalAge(channel);
}

// When a signal was set, on the wrapped iTime clock, to seed its pattern.
float ghSetAt(int channel)
{
    return iTime - signalAge(channel);
}
