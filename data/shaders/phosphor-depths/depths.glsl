// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Phosphor Depths: the gradient, the noise and the deep glow (layer 1). Both
// passes include it: glow.frag renders the deep glow into a half-resolution
// buffer, and effect.frag reads the gradient for the dust and the ember.

#ifndef PHOSPHOR_DEPTHS_GLSL
#define PHOSPHOR_DEPTHS_GLSL

#define TAU 6.28318530718

// Bounded drift: the noise domain traces a Lissajous path whose offset never
// exceeds DRIFT_RADIUS, so f32 precision never degrades into blocky facets
// however long the session runs. The rates are incommensurate, so the path
// does not visibly repeat. Every phase goes through timeAngle(), which stays
// continuous where iTime wraps.
#define DRIFT_RADIUS 4.0
#define DRIFT_W1     0.0037 // ~28 min period
#define DRIFT_W2     0.0029 // ~36 min period

// === COLOUR ===
vec3 gradientStop(int i)
{
    if (p_followTheme > 0.5) {
        // Bright cyan, blue, magenta, red: the themed equivalents of the
        // phosphor stops, so a wallpaper-driven palette carries through.
        const int index[4] = int[4](14, 12, 13, 9);
        return paletteColor(index[i]);
    }
    return i == 0 ? p_stop0.rgb : i == 1 ? p_stop1.rgb : i == 2 ? p_stop2.rgb : p_stop3.rgb;
}

// Four-stop gradient, t in [0, 1], in linear light.
vec3 fluxGradient(float t)
{
    t = clamp(t, 0.0, 1.0) * 3.0;
    vec3 c = mix(gradientStop(0), gradientStop(1), clamp(t, 0.0, 1.0));
    c = mix(c, gradientStop(2), clamp(t - 1.0, 0.0, 1.0));
    c = mix(c, gradientStop(3), clamp(t - 2.0, 0.0, 1.0));
    return srgbToLinear(c);
}

// === NOISE ===
// Sin-free hashes (Dave Hoskins): fract(sin(x) * 43758.5453) loses precision
// at large coordinates and renders as hard blocky cells once sharpened.
float hash21(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Gradient noise: value noise pins extrema to the lattice, which survives as
// straight creases once the field is sharpened. Remapped into [0, 1].
vec2 grad2(vec2 i)
{
    float a = hash21(i) * TAU;
    return vec2(cos(a), sin(a));
}

float gnoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0); // quintic, C2
    float n = mix(mix(dot(grad2(i), f), dot(grad2(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0)), u.x),
                  mix(dot(grad2(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0)),
                      dot(grad2(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0)), u.x),
                  u.y);
    return clamp(0.5 + n * 0.7, 0.0, 1.0);
}

// Each octave is rotated so no lattice axis survives into the sum.
const mat2 kOctaveRot = mat2(0.80, 0.60, -0.60, 0.80);

float fbm(vec2 p, int octaves)
{
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; i++) {
        if (i >= octaves) {
            break;
        }
        v += a * gnoise(p);
        p = kOctaveRot * p * 2.03 + 17.7;
        a *= 0.55;
    }
    return v;
}

// === LAYER 1: DEEP GLOW ===
vec3 deepGlow(vec2 uv)
{
    float aspect = iResolution.x / max(iResolution.y, 1.0);
    vec2 q = uv * vec2(aspect, 1.0) * p_depthScale;
    vec2 drift = DRIFT_RADIUS * vec2(sin(timeAngle(p_driftSpeed * DRIFT_W1)), cos(timeAngle(p_driftSpeed * DRIFT_W2)));

    // Near plane: a domain-warped field whose warp creeps too, so the clouds
    // fold rather than scroll.
    vec2 warp = vec2(fbm(q * 0.7 + vec2(drift.x * 3.0, -drift.y * 2.0), 3),
                     fbm(q * 0.7 + vec2(-drift.x * 2.5, drift.y * 3.5) + 7.3, 3));
    float body = fbm(q + (warp - 0.5) * 1.6 + vec2(drift.x, drift.y * 0.6), 4);

    // Far plane: broader, slower, dimmer; the parallax sells the depth.
    float far = fbm(q * 0.45 - vec2(drift.x * 0.5, drift.y * 0.35) + 23.1, 3);

    // A broad bloom deep below, and veins nearer the surface. A gaussian
    // ridge rather than |2n-1|, whose kink sharpens into visible creases.
    float bloom = pow(smoothstep(0.35, 0.85, body), 2.0);
    float ridge = body - 0.5;
    float filament = exp(-ridge * ridge * 42.0) * smoothstep(0.3, 0.6, far);

    // Hue from the slow far field, biased into the first stops; the last only
    // surfaces where both fields peak together.
    vec3 colBloom = fluxGradient(far * 0.75);
    vec3 colVein = fluxGradient(min(body * 0.6 + far * 0.55, 1.0));

    float farBloom = pow(smoothstep(0.45, 0.9, far), 2.0);
    vec3 result = colBloom * (bloom * 0.7 + farBloom * 0.45) + colVein * filament * p_filamentGain;
    // The field peaks around 1.5 before scaling.
    return result * p_depthIntensity * 0.9;
}

#endif // PHOSPHOR_DEPTHS_GLSL
