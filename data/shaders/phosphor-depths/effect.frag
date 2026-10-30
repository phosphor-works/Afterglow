// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Phosphor Depths: the terminal floats over a deep, dark luminous volume.
// Far below the glass, slow clouds of the phosphor gradient drift and fold.
// Depth and darkness are the point; nothing is busy, nothing sits under text.
//   1. Deep glow      a domain-warped fbm field read as two shells: a broad dim
//                     bloom (the light source far below) and ridged filaments
//                     (veins nearer the glass), over a slower far plane for
//                     parallax.
//   2. Suspended dust sparse motes hanging in the volume, brightening where the
//                     deep glow is strong.
//   3. Cursor ember   a breathing core at the cursor and a comet wake when it
//                     jumps.
// Content passes through untouched, and the volume dims to near-black under
// text-dense regions so glyphs keep their contrast.
//
// Ported from the Ghostty shader. Differences:
//  - afterglow's frame holds sRGB-encoded, premultiplied colour; light is
//    computed in linear space and encoded back, so it matches Ghostty's
//    linear framebuffer.
//  - Text is told from background by colour, and by alpha only when the
//    background really is translucent (iBackgroundOpacity), so an opaque
//    window works too.
//  - Sizes are in cells, so the look holds across font sizes and scales.
//  - The gradient is configurable or follows the terminal palette.
//  - The ember hides with the cursor and sleeps in unfocused windows.
//  - Text clearance averages a disc of taps instead of five points, which
//    printed faint dark copies of nearby glyphs into the glow.
//  - Text-dense pixels skip the noise entirely.

#define TAU 6.28318530718

// Bounded drift: the noise domain traces a Lissajous path whose offset never
// exceeds DRIFT_RADIUS, so f32 precision never degrades into blocky facets
// however long the session runs. The rates are incommensurate, so the path
// does not visibly repeat.
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
        return iPalette[index[i]];
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

// Screen-diagonal gradient coordinate: 0 top-left, 1 bottom-right (y is up).
float screenDiag(vec2 fragCoord)
{
    return (fragCoord.x + (iResolution.y - fragCoord.y)) / max(iResolution.x + iResolution.y, 1.0);
}

// How much a sample reads as content rather than background. Samples are
// premultiplied: the background arrives as bg * alpha. Compared in linear
// light, where the thresholds were tuned.
float contentness(vec4 tex)
{
    float d = distance(srgbToLinear(tex.rgb), srgbToLinear(iBackgroundColor * tex.a));
    float c = smoothstep(0.015, 0.10, d);
    // Over a translucent background, glyphs are more opaque than it is.
    if (iBackgroundOpacity < 0.97 && tex.a > iBackgroundOpacity + 0.03) {
        c = 1.0;
    }
    return c;
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
vec3 deepGlow(vec2 uv, float t)
{
    float aspect = iResolution.x / max(iResolution.y, 1.0);
    vec2 q = uv * vec2(aspect, 1.0) * p_depthScale;
    float dt = t * p_driftSpeed;
    vec2 drift = DRIFT_RADIUS * vec2(sin(dt * DRIFT_W1), cos(dt * DRIFT_W2));

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

// === LAYER 2: SUSPENDED DUST ===
vec3 suspendedDust(vec2 uv, float t, vec3 glow)
{
    float aspect = iResolution.x / max(iResolution.y, 1.0);
    vec2 g = uv * vec2(aspect, 1.0) * p_dustDensity;
    vec2 cell = floor(g);
    float seed = hash21(cell);
    if (seed < 0.72) {
        return vec3(0.0); // sparse occupancy
    }

    // A mote hangs in its cell, swaying a fraction of it; rest position and
    // sway keep the gaussian clear of the cell edge, or it reads as a square.
    vec2 rest = cell + 0.38 + 0.24 * vec2(hash21(cell + 5.1), hash21(cell + 9.7));
    vec2 sway = p_dustSway * vec2(sin(t * 0.11 * p_dustSpeed + seed * TAU), cos(t * 0.09 * p_dustSpeed + seed * 41.0));
    float d = length(g - rest - sway) * (3.2 + seed * 1.5);
    float mote = exp(-d * d * 9.0);

    // Slow glimmer; the volume's own light catches the dust.
    float glimmer = 0.55 + 0.45 * sin(t * (0.13 + seed * 0.2) * p_dustSpeed + seed * 71.0);
    float catchLight = 0.35 + 4.0 * dot(glow, vec3(1.0));
    return fluxGradient(fract(0.1 + seed * 0.8)) * mote * glimmer * catchLight * p_dustIntensity;
}

// === LAYER 3: CURSOR EMBER + JUMP WAKE ===
// Cursor rectangles are (left, top, width, height) with y up.
vec3 cursorEmber(vec2 fragCoord, float t)
{
    float presence = iCursorVisible != 0 ? 1.0 : 0.0;
    if (p_emberSleeps > 0.5) {
        // Ease into and out of sleep as the window loses and gains focus.
        float since = clamp((iTime - iTimeFocus) / 0.6, 0.0, 1.0);
        presence *= iFocus != 0 ? since : 1.0 - since;
    }
    if (presence <= 0.0) {
        return vec3(0.0);
    }

    float cellPx = max(iCellSize.y, 1.0);
    float radius = p_emberRadius * cellPx;
    vec2 cur = iCurrentCursor.xy + vec2(iCurrentCursor.z, -iCurrentCursor.w) * 0.5;
    vec2 prev = iPreviousCursor.xy + vec2(iPreviousCursor.z, -iPreviousCursor.w) * 0.5;
    vec3 tint = srgbToLinear(iCurrentCursorColor.rgb);
    vec3 coreCol = mix(fluxGradient(screenDiag(cur)), tint, p_emberTint);

    // Breathing core at the resting cursor.
    float dCore = length(fragCoord - cur);
    float breathe = 0.75 + 0.25 * sin(t * 2.1);
    float core = exp(-dCore * dCore / (radius * radius)) * breathe;
    vec3 result = coreCol * core * p_emberIntensity * 0.28;

    // Jump wake: a comet along the travel path, born when the cursor moved.
    float age = (iTime - iTimeCursorChange) / p_wakeDuration;
    float jump = distance(cur, prev);
    if (age >= 0.0 && age < 1.0 && jump > max(1.0, p_wakeMinDistance * iCellSize.x)) {
        float fade = (1.0 - age) * (1.0 - age);
        vec2 pa = fragCoord - prev;
        vec2 ba = cur - prev;
        float along = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-4), 0.0, 1.0);
        float dWake = length(pa - ba * along);
        // Thin at the origin, widest at the head; light drains toward the origin.
        float width = p_wakeWidth * cellPx * (0.35 + 0.65 * along);
        float body = exp(-dWake * dWake / (width * width));
        float drain = smoothstep(0.0, 1.0, along * 0.7 + 0.3);
        vec3 wakeCol = mix(fluxGradient(along), tint, p_emberTint);
        result += wakeCol * body * drain * fade * p_emberIntensity;

        // Landing flash at the destination as the wake dies.
        float flash = exp(-dCore * dCore / (radius * radius * 2.2)) * fade * 0.5;
        result += coreCol * flash * p_emberIntensity;
    }
    return result * presence;
}

// === TEXT CLEARANCE ===
// Where text is dense the volume dims to near-black, so it never fights glyph
// contrast. Density is averaged over a disc of taps on a golden-angle spiral:
// the same mean as a few point samples, but no single offset tap can stamp a
// dark copy of the glyphs into the glow.
#define CLEARANCE_TAPS 16
float textClearance(vec2 uv)
{
    vec2 radius = p_textClearance * max(iCellSize.y, 1.0) * vec2(1.0, 0.6) / iResolution.xy;
    float density = 0.0;
    for (int i = 0; i < CLEARANCE_TAPS; i++) {
        float r = sqrt((float(i) + 0.5) / float(CLEARANCE_TAPS));
        float a = float(i) * 2.39996323; // golden angle
        density += contentness(texture(iChannel0, uv + radius * r * vec2(cos(a), sin(a))));
    }
    return 1.0 - smoothstep(0.06, 0.45, density / float(CLEARANCE_TAPS));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec4 tex = texture(iChannel0, uv);
    float t = iTime;

    // The ember draws over everything (the cursor lives above the text); the
    // volume only lights the background.
    vec3 fx = cursorEmber(fragCoord, t);

    float isContent = contentness(tex);
    if (isContent < 1.0) {
        float clear = textClearance(uv);
        float dustWeight = mix(p_dustTextFloor, 1.0, clear);
        if (clear > 0.0 || dustWeight > 0.0) {
            vec3 glow = deepGlow(uv, t);
            vec3 dust = suspendedDust(uv, t, glow);
            fx += (glow * clear + dust * dustWeight) * (1.0 - isContent);
        }
    }

    if (all(lessThanEqual(fx, vec3(0.0)))) {
        fragColor = tex;
        return;
    }
    fragColor = vec4(linearToSrgb(srgbToLinear(tex.rgb) + fx), tex.a);
}
