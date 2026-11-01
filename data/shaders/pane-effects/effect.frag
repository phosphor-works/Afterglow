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
// passes through a portal: a ring sweeps out across it, refracting the text
// at its edge, and closes back in when it returns. While it is away, a soft
// frame in the context's colour marks it: cyan for a remote host, red for an
// elevated user, magenta for a container.
//
// Scripts play pulses over a pane with afterglow.pulse(): a portal ring, a
// flash or a shake, in a palette colour.

#include <terminal_panes.glsl>

const float Pi = 3.14159265;

vec4 backdrop() {
    return vec4(uBackgroundColor.rgb * 0.35, 1.0) * uBackgroundColor.a;
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
            vec4 ghost = closingPaneTexel(source / iResolution) * vec4(vec3(mix(1.0, facing, p_shading)), 1.0);
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
        if (hinge(pixel, r, edge, angle, source, facing)) {
            return terminalTexel(source / iResolution) * vec4(vec3(mix(1.0, facing, p_shading)), 1.0);
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
        width = smoothstep(0.0, 0.35, p);
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
    source = center + offset / vec2(width, height);
    source = clamp(source, r.xy, r.xy + r.zw - 1.0);
    return true;
}

vec4 tubeStyle(vec2 pixel, vec2 uv) {
    vec4 color = terminalTexel(uv);
    if (closingPaneActive()) {
        vec2 source;
        float glow;
        if (tube(pixel, closingPaneRect(), closingPaneProgress(), false, source, glow)) {
            vec4 ghost = closingPaneTexel(source / iResolution);
            color = vec4(ghost.rgb + glow, max(ghost.a, glow));
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
            return vec4(picture.rgb * smoothstep(0.25, 0.6, p) + glow, max(picture.a, glow));
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
    float ring = exp(-pow((length(offset) - radius) / band, 2.0)) * (1.0 - p);
    vec2 bent = pixel - normalize(offset + 1e-4) * ring * band * 0.6;
    vec4 refracted = terminalTexel(clamp(bent, r.xy, r.xy + r.zw - 1.0) / iResolution);
    color = mix(color, refracted, min(ring * 2.0, 1.0));
    color.rgb += tint * ring * max(color.a, 0.5);
    return color;
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
        // Returning home closes the portal in the theme's cursor colour.
        vec3 tint = away ? contextColor(context) : uThemeCursorColor.rgb;
        // The frame grows in as the pane leaves and fades as it returns.
        float settled = away ? smoothstep(0.0, 1.0, p) : 1.0 - smoothstep(0.0, 1.0, p);
        float edge = min(min(pixel.x - r.x, r.x + r.z - pixel.x), min(pixel.y - r.y, r.y + r.w - pixel.y));
        float frame = exp(-edge / 10.0) * p_frame * settled;
        color.rgb += tint * frame * color.a;
        // The ring opens from the pane's centre as it leaves, and closes back
        // to it as it returns.
        if (p < 1.0 && p_portal > 0.0) color = mix(color, portalRing(pixel, r, p, tint, color, away), p_portal);
    }
    return color;
}

// Scripts' pulses.
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
            color.rgb = mix(color.rgb, tint * max(color.a, 0.5), light * 0.7);
        } else if (effect == 3) {
            // A sideways shake that settles.
            float shift = sin(p * 42.0) * 9.0 * (1.0 - p) * (1.0 - p);
            vec2 source = clamp(pixel - vec2(shift, 0.0), r.xy, r.xy + r.zw - 1.0);
            color = terminalTexel(source / iResolution);
        }
    }
    return color;
}

vec4 pTerminal(vec2 uv) {
    vec2 pixel = terminalPixel(uv);
    vec4 color = int(p_style + 0.5) == 1 ? tubeStyle(pixel, uv) : hingeStyle(pixel, uv);
    return pulses(pixel, contextMarks(pixel, color));
}
