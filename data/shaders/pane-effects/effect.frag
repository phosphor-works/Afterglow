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

vec4 pTerminal(vec2 uv) {
    vec2 pixel = terminalPixel(uv);
    if (int(p_style + 0.5) == 1) return tubeStyle(pixel, uv);
    return hingeStyle(pixel, uv);
}
