// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overview Wall: the pane overview as a grid of live cards on a wall that
// curves gently toward you at its sides. The highlighted card lifts out of
// the wall. As in Pane Deck, each pixel casts one camera ray at the cards,
// keeps the two nearest and shades only those; at overviewAmount() 0 every
// card is exactly where its pane is.

#include <terminal_panes.glsl>

struct Card {
    vec3 center;
    vec3 right;
    vec3 normal;
    vec2 halfSize;
    vec4 rect;
    float focus; // 1 for the highlighted card, 0 for the rest
};

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// How close card i is to the highlight, which keeps counting past the last card.
float focusOf(int i, int n) {
    float h = mod(highlightPosition(), float(n));
    float d = abs(float(i) - h);
    return 1.0 - clamp(min(d, float(n) - d), 0.0, 1.0);
}

Card card(int i, int n, float a) {
    vec2 res = iResolution;
    vec4 r = paneRect(i);
    int columns = int(ceil(sqrt(float(n))));
    int rows = (n + columns - 1) / columns;
    vec2 cell = res * vec2(0.88, 0.8) / vec2(float(columns), float(rows));
    float fit = min(cell.x * 0.86 / max(r.z, 1.0), cell.y * 0.86 / max(r.w, 1.0));
    vec2 slot = vec2(float(i % columns), float(i / columns)) - vec2(float(columns - 1), float(rows - 1)) * 0.5;
    vec2 onWall = slot * cell;
    float radius = res.x * mix(4.0, 1.1, p_curve);
    float lift = focusOf(i, n);
    // The sides come forward and turn toward the middle.
    vec3 wall = vec3(onWall, onWall.x * onWall.x / (2.0 * radius) + lift * res.y * p_lift);
    vec3 flatCenter = vec3(r.xy + r.zw * 0.5 - res * 0.5, 0.0);
    flatCenter.x += float(paneTab(i) - currentTab()) * res.x;

    Card c;
    c.center = mix(flatCenter, wall, a);
    c.halfSize = mix(r.zw * 0.5, r.zw * 0.5 * fit * (1.0 + 0.06 * lift), a);
    float yaw = -asin(clamp(onWall.x / radius, -0.9, 0.9)) * a;
    c.right = vec3(cos(yaw), 0.0, -sin(yaw));
    c.normal = vec3(sin(yaw), 0.0, cos(yaw));
    c.rect = r;
    c.focus = lift;
    return c;
}

bool intersect(Card c, vec3 o, vec3 d, out float t, out vec2 local) {
    float denom = dot(d, c.normal);
    if (abs(denom) < 1e-5) return false;
    t = dot(c.center - o, c.normal) / denom;
    if (t <= 0.0) return false;
    vec3 p = o + d * t - c.center;
    local = vec2(dot(p, c.right), p.y);
    return true;
}

vec4 shade(int i, Card c, vec2 local, float pixel, float a, out float coverage) {
    float shown = clamp(a, 0.0, 1.0);
    float corner = min(c.halfSize.x, c.halfSize.y) * 0.05 * shown;
    float sd = sdRoundBox(local, c.halfSize, corner);
    coverage = clamp(0.5 - sd / pixel, 0.0, 1.0);
    vec2 cardUv = local / (2.0 * c.halfSize) + 0.5;
    vec2 spread = pixel / (2.0 * c.halfSize);
    vec4 color = paneTexel(i, cardUv);
    if (spread.x * c.rect.z > 1.25) {
        color = 0.25 * (paneTexel(i, cardUv + spread * vec2(-0.25, -0.25)) + paneTexel(i, cardUv + spread * vec2(0.25, -0.25))
                      + paneTexel(i, cardUv + spread * vec2(-0.25, 0.25)) + paneTexel(i, cardUv + spread * vec2(0.25, 0.25)));
    }
    color.rgb *= mix(1.0, mix(p_sideBrightness, 1.0, c.focus), shown);
    // While the overview closes, the highlight is the card it closes onto,
    // in any tab: the border stays there and fades with the wall.
    int lit = highlightedPane() >= 0 ? highlightedPane() : focusedPane();
    if (i == lit) {
        float width = c.halfSize.y * 0.025;
        float ring = (1.0 - smoothstep(width, width + pixel, -sd)) * shown;
        color = mix(color, vec4(uThemeCursorColor.rgb, 1.0), ring);
    }
    return color;
}

vec4 backdrop(vec2 uv, float a) {
    vec4 base = terminalTexel(uv);
    float radius = p_blur * a;
    if (radius < 0.5) return base;
    vec4 sum = base;
    // Each pixel turns the tap ring by its own interleaved-gradient-noise
    // angle: a fixed ring prints sharp copies of the text, one per tap.
    float turn = 6.2831853 * fract(52.9829189 * fract(dot(terminalPixel(uv), vec2(0.06711056, 0.00583715))));
    for (int k = 0; k < 12; ++k) {
        float angle = turn + 6.2831853 * (float(k) + 0.5) / 12.0;
        sum += terminalTexel(uv + terminalUv(vec2(cos(angle), sin(angle)) * radius * ((k % 2 == 0) ? 1.0 : 0.5)));
    }
    vec4 blurred = sum / 13.0;
    blurred.rgb *= mix(1.0, 1.0 - p_backdropDim, a);
    return blurred;
}

vec4 pTerminal(vec2 uv) {
    float a = overviewAmount();
    int n = min(paneCount(), 32);
    if (n == 0 || a <= 0.0005) return terminalTexel(uv);
    float shown = clamp(a, 0.0, 1.0);
    vec2 res = iResolution;
    float focal = res.y * 1.1;
    vec3 o = vec3(0.0, 0.0, focal * (1.0 + 0.1 * shown));
    vec3 d = normalize(vec3(terminalPixel(uv) - res * 0.5, -focal));

    float near = 1e20, next = 1e20;
    int nearCard = -1, nextCard = -1;
    for (int i = 0; i < n; ++i) {
        Card c = card(i, n, a);
        float t;
        vec2 local;
        if (!intersect(c, o, d, t, local)) continue;
        float pixel = t / focal / max(abs(dot(d, c.normal)), 0.15);
        if (sdRoundBox(local, c.halfSize, 0.0) > pixel) continue;
        if (t < near) { next = near; nextCard = nearCard; near = t; nearCard = i; }
        else if (t < next) { next = t; nextCard = i; }
    }
    vec4 colors[2] = vec4[2](vec4(0.0), vec4(0.0));
    for (int pick = 0; pick < 2; ++pick) {
        int i = pick == 0 ? nearCard : nextCard;
        if (i < 0) continue;
        Card c = card(i, n, a);
        float t;
        vec2 local;
        intersect(c, o, d, t, local);
        float pixel = t / focal / max(abs(dot(d, c.normal)), 0.15);
        float cover;
        vec4 color = shade(i, c, local, pixel, a, cover);
        colors[pick] = color * cover;
        // An opaque near card hides everything behind it: skip the blur.
        if (pick == 0 && colors[0].a >= 0.999) return colors[0];
    }
    vec4 result = backdrop(uv, shown);
    result = colors[1] + result * (1.0 - colors[1].a);
    return colors[0] + result * (1.0 - colors[0].a);
}
