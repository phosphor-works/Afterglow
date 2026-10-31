// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

const vec3 phCyan = vec3(0.133, 0.827, 0.933);
const vec3 phViolet = vec3(0.659, 0.333, 0.969);
const vec3 phRose = vec3(0.957, 0.247, 0.369);

float phSq(float x) { return x * x; }
float phAge(int channel) { return signalAge(channel); }
bool phLight() { return dot(uBackgroundColor.rgb, vec3(0.2126, 0.7152, 0.0722)) > 0.5; }
vec3 phTint(float x) { return mix(phCyan, phViolet, clamp(x, 0.0, 1.0)); }

// Window coordinates, y-up, from canvas pixels. The scene has one camera even
// with many panes.
vec2 phWindow(vec2 p) {
    return vec2(uTerminalRect.x + p.x, uWindowSize.y - uTerminalRect.y - p.y);
}

// Clamp to this pane, not the full backing texture, so a neighbouring
// pane's letters cannot bleed into a halo at a shared edge.
vec4 phText(vec2 p) {
    if (any(lessThan(p, vec2(0.5))) || any(greaterThan(p, iResolution - 0.5)))
        return vec4(0.0);
    return terminalTexel(terminalUv(p));
}

float phHeat() {
    float age = phAge(2);
    return signalValue(2) > 0.5 ? 1.0 - exp(-age * 4.0) : exp(-age * 2.5);
}

// Light on dark glass, coloured pigment on pale glass. Preserve opacity.
vec4 phIlluminate(vec4 c, vec3 tint, float strength) {
    strength = clamp(strength, 0.0, 0.8);
    if (phLight())
        c.rgb = mix(c.rgb, tint * 0.38 * c.a, strength);
    else
        c.rgb += tint * strength * c.a;
    return vec4(clamp(c.rgb, vec3(0.0), vec3(c.a)), c.a);
}
