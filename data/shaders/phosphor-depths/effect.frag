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
// Content passes through untouched except under the cursor ember, and the
// volume dims to near-black under text-dense regions so glyphs keep their
// contrast.
//
// Notes:
//  - The frame holds sRGB-encoded, premultiplied colour; light is computed in
//    linear space and encoded back. Light takes the frame's alpha, so the
//    volume fades with a translucent window like its background does.
//  - Text is told from background by colour, and by alpha only when the
//    background really is translucent (uBackgroundColor.a), so an opaque
//    window works too.
//  - The cursor and text-clearance sizes are in cells, so they hold across
//    font sizes and scales. The clouds and the dust scale with the canvas.
//  - The gradient is configurable or follows the terminal palette.
//  - The ember hides with the cursor and sleeps in unfocused panes and
//    windows.
//  - Text clearance averages a disc of taps instead of five points, which
//    printed faint dark copies of nearby glyphs into the glow.
//  - Text-dense pixels skip the dust and the glow entirely.
//  - The deep glow renders at half resolution (glow.frag, bufferScale 0.5):
//    a quarter of the noise per frame, which matters at 4K. The clouds are
//    smooth enough that previews match full resolution within 2/255, and
//    within 6/255 at the finest cloud scale.

#include <terminal_multipass.glsl>
#include "depths.glsl"

// Screen-diagonal gradient coordinate: 0 top-left, 1 bottom-right.
float screenDiag(vec2 pixel)
{
    return (pixel.x + pixel.y) / max(iResolution.x + iResolution.y, 1.0);
}

// How much a sample reads as content rather than background. Samples are
// premultiplied: the background arrives as bg * alpha. Compared in linear
// light, where the thresholds were tuned.
float contentness(vec4 tex)
{
    float d = distance(srgbToLinear(tex.rgb), srgbToLinear(uBackgroundColor.rgb * tex.a));
    float c = smoothstep(0.015, 0.10, d);
    // Over a translucent background, glyphs are more opaque than it is.
    if (uBackgroundColor.a < 0.97 && tex.a > uBackgroundColor.a + 0.03) {
        c = 1.0;
    }
    return c;
}

// === LAYER 2: SUSPENDED DUST ===
vec3 suspendedDust(vec2 uv, vec3 glow)
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
    vec2 sway = p_dustSway * vec2(sin(timeAngle(0.11 * p_dustSpeed) + seed * TAU), cos(timeAngle(0.09 * p_dustSpeed) + seed * 41.0));
    float d = length(g - rest - sway) * (3.2 + seed * 1.5);
    float mote = exp(-d * d * 9.0);

    // Slow glimmer; the volume's own light catches the dust.
    float glimmer = 0.55 + 0.45 * sin(timeAngle((0.13 + seed * 0.2) * p_dustSpeed) + seed * 71.0);
    float catchLight = 0.35 + 4.0 * dot(glow, vec3(1.0));
    return fluxGradient(fract(0.1 + seed * 0.8)) * mote * glimmer * catchLight * p_dustIntensity;
}

// === LAYER 3: CURSOR EMBER + JUMP WAKE ===
vec3 cursorEmber(vec2 pixel)
{
    float presence = cursorVisible() ? 1.0 : 0.0;
    if (p_emberSleeps > 0.5) {
        // Ease into and out of sleep as the pane or window loses and gains focus.
        float since = clamp(focusAge() / 0.6, 0.0, 1.0);
        presence *= paneFocused() ? since : 1.0 - since;
    }
    if (presence <= 0.0) {
        return vec3(0.0);
    }

    float cellPx = max(uCellSize.y, 1.0);
    float radius = p_emberRadius * cellPx;
    vec2 cur = cursorCenter();
    vec2 prev = previousCursorCenter();
    vec3 tint = srgbToLinear(uCursorColor.rgb);
    vec3 coreCol = mix(fluxGradient(screenDiag(cur)), tint, p_emberTint);

    // Breathing core at the resting cursor.
    float dCore = length(pixel - cur);
    float breathe = 0.75 + 0.25 * sin(timeAngle(2.1));
    float core = exp(-dCore * dCore / (radius * radius)) * breathe;
    vec3 result = coreCol * core * p_emberIntensity * 0.28;

    // Jump wake: a comet along the travel path, born when the cursor moved.
    float age = cursorAge() / p_wakeDuration;
    float jump = distance(cur, prev);
    if (age < 1.0 && jump > max(1.0, p_wakeMinDistance * uCellSize.x)) {
        float fade = (1.0 - age) * (1.0 - age);
        vec2 pa = pixel - prev;
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
    vec2 radius = terminalUv(p_textClearance * max(uCellSize.y, 1.0) * vec2(1.0, 0.6));
    float density = 0.0;
    for (int i = 0; i < CLEARANCE_TAPS; i++) {
        float r = sqrt((float(i) + 0.5) / float(CLEARANCE_TAPS));
        float a = float(i) * 2.39996323; // golden angle
        density += contentness(terminalTexel(uv + radius * r * vec2(cos(a), sin(a))));
    }
    return 1.0 - smoothstep(0.06, 0.45, density / float(CLEARANCE_TAPS));
}

vec4 pTerminal(vec2 uv)
{
    vec4 tex = terminalTexel(uv);

    // The ember draws over everything (the cursor lives above the text); the
    // volume only lights the background.
    vec3 fx = cursorEmber(terminalPixel(uv));

    float isContent = contentness(tex);
    if (isContent < 1.0) {
        float clear = textClearance(uv);
        float dustWeight = mix(p_dustTextFloor, 1.0, clear);
        if (clear > 0.0 || dustWeight > 0.0) {
            vec3 glow = texture(iChannel0, uv).rgb;
            vec3 dust = suspendedDust(uv, glow);
            fx += (glow * clear + dust * dustWeight) * (1.0 - isContent);
        }
    }

    if (all(lessThanEqual(fx, vec3(0.0)))) {
        return tex;
    }
    // The transfer functions work on straight colour; the added light is
    // premultiplied by the frame's alpha. The ember core and the wake head can
    // pass 1: clamp the straight colour there, as an opaque target would, so
    // rgb never exceeds alpha in a translucent window.
    vec4 linear = toLinearPremultiplied(tex);
    return toSrgbPremultiplied(vec4(min(linear.rgb + fx * tex.a, vec3(tex.a)), tex.a));
}
