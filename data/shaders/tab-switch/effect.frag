// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tab Switch: the tab transition. The new tab is this pass's input and the
// tab being left comes from previousTabTexel(); both keep running. Four
// styles share one camera, which at progress 0 shows the old tab exactly as
// the window did and at progress 1 the new one:
//   0 cube       the tabs are neighbouring faces of a box that turns
//   1 page flip  the old tab lifts off its edge like a page
//   2 shatter    the old tab breaks into shards that fall in a wave
//   3 slide      the tabs slide past each other, the old one sinking back

#include <terminal_panes.glsl>

const float Pi = 3.14159265;

struct Quad {
    vec3 center;
    vec3 right;
    vec3 normal;
    vec2 halfSize;
};

vec3 rotateY(vec3 v, float a) {
    float c = cos(a), s = sin(a);
    return vec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

// Where the ray meets the quad: distance, texture uv, and whether it sees the front.
bool hitQuad(Quad q, vec3 o, vec3 d, out float t, out vec2 uv, out bool front) {
    float denom = dot(d, q.normal);
    if (abs(denom) < 1e-5) return false;
    t = dot(q.center - o, q.normal) / denom;
    if (t <= 0.0) return false;
    vec3 p = o + d * t - q.center;
    vec2 local = vec2(dot(p, q.right), p.y);
    if (any(greaterThan(abs(local), q.halfSize))) return false;
    uv = local / (2.0 * q.halfSize) + 0.5;
    front = denom < 0.0;
    return true;
}

// Light falls from the camera: faces turned away from it darken.
float light(vec3 normal) {
    return mix(1.0, 0.25 + 0.75 * max(normal.z, 0.0), p_shading);
}

vec4 backdrop() {
    return vec4(uBackgroundColor.rgb * 0.2, 1.0) * uBackgroundColor.a;
}

vec4 cube(vec2 uv, float progress, float dir) {
    vec2 res = iResolution;
    float focal = res.y * 1.1;
    float width = res.x;
    float angle = -dir * progress * Pi * 0.5;
    vec3 eye = vec3(0.0, 0.0, focal + sin(clamp(progress, 0.0, 1.0) * Pi) * width * p_depth);
    vec3 d = normalize(vec3(terminalPixel(uv) - res * 0.5, -focal));
    vec3 center = vec3(0.0, 0.0, -width * 0.5);
    Quad from = Quad(center + rotateY(vec3(0.0, 0.0, width * 0.5), angle), rotateY(vec3(1.0, 0.0, 0.0), angle),
                     rotateY(vec3(0.0, 0.0, 1.0), angle), res * 0.5);
    Quad to = Quad(center + rotateY(vec3(dir * width * 0.5, 0.0, 0.0), angle), rotateY(vec3(0.0, 0.0, -dir), angle),
                   rotateY(vec3(dir, 0.0, 0.0), angle), res * 0.5);
    float tFrom, tTo;
    vec2 uvFrom, uvTo;
    bool frontFrom, frontTo;
    bool hitFrom = hitQuad(from, eye, d, tFrom, uvFrom, frontFrom) && frontFrom;
    bool hitTo = hitQuad(to, eye, d, tTo, uvTo, frontTo) && frontTo;
    if (hitTo && (!hitFrom || tTo < tFrom)) return terminalTexel(uvTo) * vec4(vec3(light(to.normal)), 1.0);
    if (hitFrom) return previousTabTexel(uvFrom) * vec4(vec3(light(from.normal)), 1.0);
    return backdrop();
}

vec4 pageFlip(vec2 uv, float progress, float dir) {
    vec2 res = iResolution;
    float focal = res.y * 1.1;
    float width = res.x;
    float turn = clamp(progress, 0.0, 1.0) * Pi;
    float angle = -dir * turn;
    // The page swings toward the camera: pull back so it stays in view.
    vec3 eye = vec3(0.0, 0.0, focal + sin(turn) * width * (0.3 + p_depth));
    vec3 d = normalize(vec3(terminalPixel(uv) - res * 0.5, -focal));
    vec3 hinge = vec3(-dir * width * 0.5, 0.0, 0.0);
    Quad page = Quad(hinge + rotateY(vec3(dir * width * 0.5, 0.0, 0.0), angle), rotateY(vec3(1.0, 0.0, 0.0), angle),
                     rotateY(vec3(0.0, 0.0, 1.0), angle), res * 0.5);
    float t;
    vec2 pageUv;
    bool front;
    if (hitQuad(page, eye, d, t, pageUv, front)) {
        if (front) return previousTabTexel(pageUv) * vec4(vec3(light(page.normal)), 1.0);
        // The back of the page: the old tab showing faintly through.
        return vec4(mix(backdrop().rgb, previousTabTexel(pageUv).rgb, 0.18), 1.0);
    }
    // The new tab lies under the page, in its shadow while the page is up.
    vec3 under = eye + d * ((0.0 - eye.z) / d.z);
    vec2 underUv = (under.xy + res * 0.5) / res;
    if (any(lessThan(underUv, vec2(0.0))) || any(greaterThan(underUv, vec2(1.0)))) return backdrop();
    float shadow = 1.0 - 0.45 * p_shading * sin(turn);
    return terminalTexel(underUv) * vec4(vec3(shadow), 1.0);
}

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

vec4 shatter(vec2 uv, float progress, float dir) {
    vec2 res = iResolution;
    vec2 pixel = terminalPixel(uv);
    const float Columns = 12.0;
    float rows = max(2.0, floor(Columns * res.y / res.x + 0.5));
    vec2 cell = res / vec2(Columns, rows);
    float p = clamp(progress, 0.0, 1.0);
    vec2 home = floor(pixel / cell);
    // Shards only fall and drift a little: the pixel can show a shard from
    // its own column or a neighbour, from its own row or any row above.
    for (float row = home.y; row >= 0.0; row -= 1.0) {
        for (float dx = -1.0; dx <= 1.0; dx += 1.0) {
            vec2 id = vec2(home.x + dx, row);
            if (id.x < 0.0 || id.x >= Columns) continue;
            float r1 = hash(id), r2 = hash(id + 17.3), r3 = hash(id + 41.9);
            // A wave crosses the tab against the direction of travel.
            float across = dir > 0.0 ? id.x / Columns : 1.0 - id.x / Columns;
            float delay = across * 0.45 + r1 * 0.2;
            float t = clamp((p - delay) / 0.35, 0.0, 1.0);
            vec2 center = (id + 0.5) * cell;
            vec2 offset = vec2((r2 - 0.5) * cell.x * 1.5 - dir * cell.x * 0.8, res.y * 1.2 * t * t) * t;
            float spin = (r3 - 0.5) * 2.4 * t;
            float scale = 1.0 - 0.25 * t;
            vec2 q = pixel - center - offset;
            q = mat2(cos(spin), sin(spin), -sin(spin), cos(spin)) * q / scale;
            // Cracks open as the shard leaves.
            vec2 halfCell = cell * 0.5 - t * 1.5;
            if (all(lessThanEqual(abs(q), halfCell))) {
                vec2 sourceUv = (center + q) / res;
                float fade = 1.0 - smoothstep(0.65, 1.0, t);
                float edge = mix(1.0, 0.7 + 0.3 * cos(spin * 2.0), p_shading);
                return mix(terminalTexel(uv) * mix(0.35, 1.0, p), previousTabTexel(sourceUv) * vec4(vec3(edge), 1.0), fade);
            }
        }
    }
    return terminalTexel(uv) * vec4(vec3(mix(0.35, 1.0, p)), 1.0);
}

vec4 slide(vec2 uv, float progress, float dir) {
    vec2 res = iResolution;
    vec2 pixel = terminalPixel(uv);
    // The new tab slides in flat; the old one slides out, sinking back.
    vec2 newPixel = pixel - vec2(dir * (1.0 - progress) * res.x, 0.0);
    if (all(greaterThanEqual(newPixel, vec2(0.0))) && all(lessThan(newPixel, res))) return terminalTexel(newPixel / res);
    float sink = 1.0 - 0.12 * clamp(progress, 0.0, 1.0);
    vec2 center = res * 0.5 + vec2(-dir * progress * res.x * 0.5, 0.0);
    vec2 oldPixel = (pixel - center) / sink + res * 0.5;
    if (all(greaterThanEqual(oldPixel, vec2(0.0))) && all(lessThan(oldPixel, res))) {
        return previousTabTexel(oldPixel / res) * vec4(vec3(mix(1.0, 0.5, p_shading * clamp(progress, 0.0, 1.0))), 1.0);
    }
    return backdrop();
}

vec4 pTerminal(vec2 uv) {
    if (!tabTransitionActive()) return terminalTexel(uv);
    float progress = tabTransitionProgress();
    float dir = tabTransitionDirection();
    int style = int(p_style + 0.5);
    if (style == 1) return pageFlip(uv, progress, dir);
    if (style == 2) return shatter(uv, progress, dir);
    if (style == 3) return slide(uv, progress, dir);
    return cube(uv, progress, dir);
}
