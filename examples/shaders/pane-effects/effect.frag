// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Pane Effects: panes opening from a split and closing. The input holds every
// open pane where it is; a closed pane's last frame comes from
// closingPaneTexel() at closingPaneRect(). Two styles:
//   0 hinge  a new pane swings in from the edge it split from; a closed one
//            falls back off its bottom edge
//   1 tube   panes switch on and off like a picture tube: a bright line that
//            opens into the picture, and the picture squeezing back to a dot
// Progress is linear; each style eases it. At progress 1 nothing changes.
//
// In both styles, a pane whose shell moves to another host, user or container
// passes through a portal. Two portal styles:
//   0 ring    a ring sweeps out across the pane, refracting the text at its
//             edge, and closes back in when the pane returns
//   1 vortex  a black hole opens at the pane's centre, the text spirals into
//             it, and comes back out of it; it turns the other way when the
//             pane returns
// While the pane is away, a soft frame in the context's colour marks it: the
// theme's bright cyan for a remote host, bright red for an elevated user,
// bright magenta for a container. The frame fades out in that colour when
// the pane returns.
//
// Scripts play pulses over a pane with afterglow.pulse(): a portal ring, a
// flash or a shake, in a palette colour.

#include <terminal_panes.glsl>

const float Pi = 3.14159265;

vec4 backdrop() {
    return vec4(uBackgroundColor.rgb * 0.35, 1.0) * uBackgroundColor.a;
}

// Adds light to a premultiplied colour. Alpha rises to cover the light, so
// the colour stays valid over a translucent window.
vec4 addLight(vec4 color, vec3 light) {
    float a = clamp(max(color.a, max(light.r, max(light.g, light.b))), 0.0, 1.0);
    return vec4(min(color.rgb + light, vec3(a)), a);
}

// A window pixel clamped half a pixel inside r, so the linear sampler takes
// nothing from the neighbouring pane or divider.
vec2 clampToRect(vec2 pixel, vec4 r) {
    return clamp(pixel, r.xy + 0.5, r.xy + r.zw - 0.5);
}

bool inside(vec2 p, vec4 r) {
    return all(greaterThanEqual(p, r.xy)) && all(lessThan(p, r.xy + r.zw));
}

// The pane rect r turned by angle about its edge (1 left, 2 top, 3 right,
// 4 bottom), away from the camera. Where the pixel's ray meets it, returns
// the window pixel of the pane's content there, and how squarely it faces.
bool hinge(vec2 pixel, vec4 r, int edge, float angle, out vec2 source, out float facing) {
    vec2 res = iResolution;
    float focal = res.y * 1.1;
    vec3 eye = vec3(0.0, 0.0, focal);
    vec3 d = normalize(vec3(pixel - res * 0.5, -focal));
    // The hinge, the direction away from it in the window, and along it.
    vec2 hingePoint = edge == 1 ? r.xy : edge == 2 ? r.xy : edge == 3 ? vec2(r.x + r.z, r.y) : vec2(r.x, r.y + r.w);
    vec2 away = edge == 1 ? vec2(1.0, 0.0) : edge == 2 ? vec2(0.0, 1.0) : edge == 3 ? vec2(-1.0, 0.0) : vec2(0.0, -1.0);
    vec2 along = edge == 1 || edge == 3 ? vec2(0.0, 1.0) : vec2(1.0, 0.0);
    float reach = edge == 1 || edge == 3 ? r.z : r.w;
    float span = edge == 1 || edge == 3 ? r.w : r.z;
    vec3 h = vec3(hingePoint - res * 0.5, 0.0);
    vec3 a = vec3(away * cos(angle), -sin(angle));
    vec3 b = vec3(along, 0.0);
    vec3 n = cross(a, b);
    float denom = dot(d, n);
    if (abs(denom) < 1e-5) return false;
    float t = dot(h - eye, n) / denom;
    if (t <= 0.0) return false;
    vec3 p = eye + d * t - h;
    float u = dot(p, a);
    float v = dot(p, b);
    // v runs along the hinge from its start corner.
    if (u < 0.0 || u > reach || v < 0.0 || v > span) return false;
    source = hingePoint + away * u + along * v;
    facing = abs(cos(angle));
    return true;
}

vec4 hingeStyle(vec2 pixel, vec2 uv) {
    vec4 color = terminalTexel(uv);
    // Closing: the last frame falls back off its bottom edge and fades.
    if (closingPaneActive()) {
        float p = closingPaneProgress();
        vec2 source;
        float facing;
        if (hinge(pixel, closingPaneRect(), 4, p * p * Pi * 0.48, source, facing)) {
            vec4 ghost = closingPaneTexel(clampToRect(source, closingPaneRect()) / iResolution) * vec4(vec3(mix(1.0, facing, p_shading)), 1.0);
            color = mix(color, ghost, 1.0 - p * p);
        }
    }
    // Opening: new panes swing in from the edge they split from.
    for (int i = 0; i < paneCount(); ++i) {
        float p = paneOpening(i);
        int edge = paneOpenEdge(i);
        if (p >= 1.0 || edge == 0) continue;
        vec4 r = paneRect(i);
        float settle = 1.0 - p;
        float angle = settle * settle * settle * Pi * 0.45;
        vec2 source;
        float facing;
        // Only inside its own place: where perspective carries the turned plane
        // over a neighbour, the neighbour (at the glass) hides it.
        if (inside(pixel, r) && hinge(pixel, r, edge, angle, source, facing)) {
            return terminalTexel(clampToRect(source, r) / iResolution) * vec4(vec3(mix(1.0, facing, p_shading)), 1.0);
        }
        if (inside(pixel, r)) return backdrop();
    }
    return color;
}

// One frame of a picture tube switching on (on = 1) or off (on = 0) at
// progress p over the rect r. Returns the content's pixel and the added glow.
bool tube(vec2 pixel, vec4 r, float p, bool on, out vec2 source, out float glow) {
    vec2 center = r.xy + r.zw * 0.5;
    // On: a line grows across, then opens into the picture.
    // Off: the picture squeezes to a line, then the line to a dot that fades.
    float width, height;
    if (on) {
        // At least one pixel wide, so the division below stays finite.
        width = max(smoothstep(0.0, 0.35, p), 1.0 / max(r.z, 1.0));
        height = mix(2.0 / r.w, 1.0, smoothstep(0.3, 1.0, p));
        glow = (1.0 - smoothstep(0.3, 1.0, p)) * p_glow;
    } else {
        height = mix(1.0, 2.0 / r.w, smoothstep(0.0, 0.55, p));
        width = 1.0 - smoothstep(0.55, 0.85, p) * 0.995;
        glow = smoothstep(0.0, 0.55, p) * (1.0 - smoothstep(0.85, 1.0, p)) * p_glow;
    }
    vec2 halfSize = r.zw * 0.5 * vec2(width, height);
    vec2 offset = pixel - center;
    if (any(greaterThan(abs(offset), halfSize + 0.5))) return false;
    source = clampToRect(center + offset / vec2(width, height), r);
    return true;
}

vec4 tubeStyle(vec2 pixel, vec2 uv) {
    vec4 color = terminalTexel(uv);
    if (closingPaneActive()) {
        vec2 source;
        float glow;
        float p = closingPaneProgress();
        if (tube(pixel, closingPaneRect(), p, false, source, glow)) {
            // The dot fades with its glow, so nothing pops off at the end.
            vec4 ghost = addLight(closingPaneTexel(source / iResolution), vec3(glow));
            color = mix(color, ghost, 1.0 - smoothstep(0.85, 1.0, p));
        }
    }
    for (int i = 0; i < paneCount(); ++i) {
        float p = paneOpening(i);
        if (p >= 1.0 || paneOpenEdge(i) == 0) continue;
        vec4 r = paneRect(i);
        if (!inside(pixel, r)) continue;
        vec2 source;
        float glow;
        if (tube(pixel, r, p, true, source, glow)) {
            vec4 picture = terminalTexel(source / iResolution);
            return addLight(vec4(picture.rgb * smoothstep(0.25, 0.6, p), picture.a), vec3(glow));
        }
        return backdrop();
    }
    return color;
}

// A ring sweeping out from the pane's centre, refracting what it crosses.
vec4 portalRing(vec2 pixel, vec4 r, float p, vec3 tint, vec4 color, bool outward) {
    vec2 center = r.xy + r.zw * 0.5;
    float reach = length(r.zw) * 0.55;
    float eased = 1.0 - pow(1.0 - p, 3.0);
    float radius = reach * (outward ? eased : 1.0 - eased);
    float band = min(r.z, r.w) * 0.06;
    vec2 offset = pixel - center;
    // pow() is undefined for a negative base: square the distance instead.
    float k = (length(offset) - radius) / band;
    float ring = exp(-k * k) * (1.0 - p);
    vec2 bent = pixel - normalize(offset + 1e-4) * ring * band * 0.6;
    vec4 refracted = terminalTexel(clampToRect(bent, r) / iResolution);
    color = mix(color, refracted, min(ring * 2.0, 1.0));
    return addLight(color, tint * ring * max(color.a, 0.5));
}

// One value in 0..1 for each cell of a grid: the stars behind the vortex.
float hash(vec2 cell) {
    vec3 q = fract(vec3(cell.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

// What shows where the text has left: the background, darker and more opaque,
// with a few stars. `at` is a point of the plane that falls into the hole, so
// the stars streak toward the hole as the text does.
vec4 deepSpace(vec2 at, vec3 tint) {
    vec4 dark = vec4(uBackgroundColor.rgb * 0.1, 1.0) * mix(uBackgroundColor.a, 1.0, 0.7);
    vec2 cell = at / 30.0;
    vec2 id = floor(cell);
    float chance = hash(id);
    vec2 spot = 0.2 + 0.6 * vec2(hash(id + 17.0), hash(id + 31.0));
    float star = smoothstep(0.14, 0.0, length(fract(cell) - spot)) * smoothstep(0.78, 1.0, chance);
    return addLight(dark, mix(vec3(1.0), tint, 0.35) * star);
}

// v turned by angle, the way that makes a growing angle wind the vortex in.
vec2 turned(vec2 v, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return vec2(v.x * c + v.y * s, v.y * c - v.x * s);
}

// The pane falling into a black hole at its centre and coming back out. The
// hole opens, the text spirals into it, and midway only the hole shows, with
// its disc flaring. Then the text spirals back out and the hole closes.
// spin is 1 or -1, the way it turns. The progress is the only clock, so the
// pack reads no iTime.
vec4 vortex(vec2 pixel, vec4 r, float p, vec3 tint, float spin) {
    vec2 center = r.xy + r.zw * 0.5;
    float reach = 0.5 * length(r.zw);
    vec2 offset = pixel - center;
    float dist = length(offset);
    vec2 outward = dist > 1e-3 ? offset / dist : vec2(1.0, 0.0);
    // How deep the pane is in the hole: 0 at both ends, 1 midway. It falls
    // faster as it goes in, and slows as it comes out.
    float fall = 1.0 - abs(2.0 * p - 1.0);
    float depth = fall * fall;

    // A point at radius a (in reaches) falls as a body from rest does:
    // a^1.5 shrinks at a constant rate, so the centre goes first and the text
    // stretches along the radius. At a pull of 1 the corners arrive: past it,
    // only the hole shows, and the turn below changes sign unseen.
    float pull = 1.7 * depth;
    float inner = pow(dist / reach, 1.5);
    float from = pow(inner + pull, 2.0 / 3.0);
    // 1 where the text is as it was, toward 0 where it is squeezed around
    // the hole, too fine for the sampler: there it takes the portal's colour
    // and goes dark.
    float squeeze = pow((inner + 1e-6) / (inner + pull + 1e-6), 2.0 / 3.0);
    // The inside turns faster. Coming out, it unwinds from the other side,
    // so the pane keeps turning the same way through the hole.
    float shear = spin * p_swirl / (0.3 + 2.0 * dist / reach);
    vec2 source = center + turned(outward, (p < 0.5 ? 2.6 : -2.6) * depth * shear) * from * reach;
    // The stars show through the midpoint, where the text's turn changes
    // sign, so theirs follows the progress and never jumps.
    vec2 sky = center + turned(outward, 3.0 * p * shear) * from * reach;

    vec4 text = terminalTexel(clampToRect(source, r) / iResolution);
    float luma = dot(text.rgb, vec3(0.2126, 0.7152, 0.0722));
    text.rgb = mix(tint * luma, text.rgb, smoothstep(0.25, 0.9, squeeze)) * smoothstep(0.06, 0.45, squeeze);
    // How far inside the pane's edge the source is, in pixels: the edge of
    // the falling text is soft, and glows.
    vec2 margin = min(source - r.xy, r.xy + r.zw - source);
    float within = min(margin.x, margin.y);
    vec4 color = mix(deepSpace(sky, tint) * vec4(vec3(smoothstep(0.02, 0.3, squeeze)), 1.0), text, smoothstep(-1.0, 1.0, within));
    float rim = exp(-abs(within) / 6.0) * smoothstep(0.0, 0.15, depth) * squeeze;

    // The hole: a black disc, a thin bright ring at its edge, and a disc of
    // light in two spiral arms, with finer streaks, that turn as the progress
    // runs. The disc flares midway, while the pane is through.
    float open = pow(depth, 0.4);
    float horizon = 0.3 * 0.5 * min(r.z, r.w) * open;
    float beyond = max(dist - horizon, 0.0) / max(horizon, 1.0);
    float ring = exp(-pow((dist - horizon * 1.1) / (horizon * 0.07 + 1.0), 2.0));
    float angle = atan(offset.y, offset.x);
    float wind = spin * log(beyond + 0.35);
    float arms = 0.5 + 0.5 * sin(2.0 * angle + 5.0 * wind - spin * p * 14.0);
    float streaks = 0.5 + 0.5 * sin(7.0 * angle + 23.0 * wind - spin * p * 31.0);
    float disc = exp(-beyond * 1.3) * smoothstep(0.0, 0.2, beyond) * (0.25 + 0.75 * arms) * (0.6 + 0.4 * streaks);
    float flare = 1.0 + 1.6 * smoothstep(0.5, 1.0, depth);
    vec3 light = (tint * (disc * flare + rim * 0.6) + mix(tint, vec3(1.0), 0.6) * ring) * open;
    color = addLight(color, light);
    return mix(color, vec4(0.0, 0.0, 0.0, 1.0), smoothstep(horizon + 1.0, horizon - 1.0, dist) * step(1.0, horizon));
}

vec3 contextColor(int context) {
    return context == 2 ? paletteColor(9) : context == 3 ? paletteColor(13) : paletteColor(14);
}

// Portals and frames for panes away from here, over what the style drew.
vec4 contextMarks(vec2 pixel, vec4 color) {
    for (int i = 0; i < paneCount(); ++i) {
        if (!paneInCurrentTab(i)) continue;
        int context = paneContext(i);
        float p = paneContextChange(i);
        if (context == 0 && p >= 1.0) continue;
        vec4 r = paneRect(i);
        if (!inside(pixel, r)) continue;
        bool away = context != 0;
        int previous = panePreviousContext(i);
        // Returning home closes the ring in the theme's cursor colour.
        vec3 tint = away ? contextColor(context) : uThemeCursorColor.rgb;
        vec3 was = previous != 0 ? contextColor(previous) : tint;
        // The portal: the vortex takes the pane in and gives it back. When the
        // pane returns, it turns the other way, in the colour of the place
        // that the pane leaves. The ring opens from the pane's centre as it
        // leaves, and closes back to it as it returns.
        if (p < 1.0 && p_portal > 0.0) {
            vec4 through = int(p_portalStyle + 0.5) == 1 ? vortex(pixel, r, p, away ? tint : was, away ? 1.0 : -1.0)
                                                         : portalRing(pixel, r, p, tint, color, away);
            color = mix(color, through, p_portal);
        }
        // The frame grows in as the pane leaves and fades as it returns, in
        // the colour it had. Between two contexts it stays and changes colour.
        // It is at the pane's edge, not on the text that the portal moves.
        float q = smoothstep(0.0, 1.0, p);
        vec3 frameTint = away ? mix(was, tint, q) : was;
        float settled = away ? (previous != 0 ? 1.0 : q) : 1.0 - q;
        float edge = min(min(pixel.x - r.x, r.x + r.z - pixel.x), min(pixel.y - r.y, r.y + r.w - pixel.y));
        float frame = exp(-edge / 10.0) * p_frame * settled;
        color = addLight(color, frameTint * frame * color.a);
    }
    return color;
}

// Scripts' pulses. A shake reads the input again, so pulses come before the
// context marks, which then still frame a shaking pane.
vec4 pulses(vec2 pixel, vec4 color) {
    for (int i = 0; i < paneCount(); ++i) {
        float p = panePulse(i);
        if (p >= 1.0 || !paneInCurrentTab(i)) continue;
        vec4 r = paneRect(i);
        if (!inside(pixel, r)) continue;
        int effect = panePulseEffect(i);
        vec3 tint = panePulseColor(i);
        if (effect == 1) {
            color = portalRing(pixel, r, p, tint, color, true);
        } else if (effect == 2) {
            // A flash that lights the pane and fades, strongest at the edges.
            float edge = min(min(pixel.x - r.x, r.x + r.z - pixel.x), min(pixel.y - r.y, r.y + r.w - pixel.y));
            float light = (1.0 - p) * (1.0 - p) * (0.35 + 0.65 * exp(-edge / 24.0));
            color = mix(color, vec4(tint, 1.0) * max(color.a, 0.5), light * 0.7);
        } else if (effect == 3) {
            // A sideways shake that settles.
            float shift = sin(p * 42.0) * 9.0 * (1.0 - p) * (1.0 - p);
            color = terminalTexel(clampToRect(pixel - vec2(shift, 0.0), r) / iResolution);
        }
    }
    return color;
}

vec4 pTerminal(vec2 uv) {
    vec2 pixel = terminalPixel(uv);
    vec4 color = int(p_style + 0.5) == 1 ? tubeStyle(pixel, uv) : hingeStyle(pixel, uv);
    return contextMarks(pixel, pulses(pixel, color));
}
