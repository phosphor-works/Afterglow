// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Pane Deck: the pane overview. Each pane of every tab is a card in 3D,
// textured with the pane's live pixels, on a carousel whose highlighted card
// faces the camera. Each tab's cards form a group, with a gap between groups.
// Other tabs wait one window width per tab to the side, like pages, so they
// slide in as the deck opens. Every pixel casts one camera ray at the cards and keeps
// the two nearest hits, so card edges blend smoothly over what is behind.
// Behind the cards, the window is blurred and dimmed, and a floor reflects
// the cards.
//
// At overviewAmount() 0 the camera and every card coincide with the flat
// window, so opening and closing start and end exactly on the window.

#include <terminal_color.glsl>
#include <terminal_panes.glsl>

const float Pi = 3.14159265;

struct Card {
    vec3 center;
    vec3 right;
    vec3 normal;
    vec2 halfSize;
    vec4 rect;      // the pane in window px
    float offset;   // signed distance from the highlight, in cards
    float angle;    // its place round the carousel, in radians
};

struct Camera {
    vec3 eye;
    vec3 forward;
    vec3 right;
    vec3 down;
    float focal;
};

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// How much a pane scales to its deck size: every card the same height, at
// most 62% of the window wide.
float deckScale(vec4 r) {
    return min(iResolution.y * p_cardSize / max(r.w, 1.0), iResolution.x * 0.62 / max(r.z, 1.0));
}

// The widest card in the deck, which sets the carousel's radius.
float widestCard(int n) {
    float widest = 1.0;
    for (int i = 0; i < n; ++i) {
        vec4 r = paneRect(i);
        widest = max(widest, r.z * deckScale(r));
    }
    return widest;
}

// A card's place on the carousel, with a gap before each new tab.
float slot(int i) {
    return float(i) + float(paneTab(i) - paneTab(0)) * p_tabGap;
}

// The carousel's shape: slots round the ring, the angle between them, and
// the highlight's slot. The same for every card, so computed once a pixel.
struct Ring {
    float slots;
    float step;
    float highlight;
};

Ring ring() {
    int n = max(paneCount(), 1);
    Ring g;
    g.slots = slot(n - 1) + 1.0 + (paneTab(n - 1) != paneTab(0) ? p_tabGap : 0.0);
    g.step = 2.0 * Pi / max(g.slots, 7.0);
    float h = highlightPosition();
    int below = clamp(int(floor(h)), 0, n - 1);
    g.highlight = mix(slot(below), slot(min(below + 1, n - 1)), clamp(h - float(below), 0.0, 1.0));
    return g;
}

// A card's signed distance from the highlight round the ring, in slots. It
// wraps the short way round the whole ring, whose seam is directly behind
// the highlight: a ring with few cards is partly empty, so the seam never
// falls on a card at the side.
float ringOffset(int i, Ring g) {
    float k = slot(i) - g.highlight;
    float ringSlots = 2.0 * Pi / g.step;
    return k - ringSlots * floor(k / ringSlots + 0.5);
}

Card card(int i, float a, float widest, Ring g) {
    vec2 res = iResolution;
    vec4 r = paneRect(i);
    // Flat: where the pane is in its tab, with other tabs to the side.
    vec3 flatCenter = vec3(r.xy + r.zw * 0.5 - res * 0.5, 0.0);
    flatCenter.x += float(paneTab(i) - currentTab()) * res.x;
    vec2 flatHalf = r.zw * 0.5;
    float fit = deckScale(r);
    float radius = widest * p_spacing / (2.0 * sin(g.step * 0.5));
    float k = ringOffset(i, g);
    float theta = k * g.step;
    vec3 deckCenter = vec3(radius * sin(theta), -res.y * 0.04, radius * (cos(theta) - 1.0));

    Card c;
    c.center = mix(flatCenter, deckCenter, a);
    c.halfSize = mix(flatHalf, flatHalf * fit, a);
    // Cards turn partly toward the camera, so the neighbours stay readable.
    // sin() keeps the turn continuous where a card crosses the seam behind.
    float yaw = 0.6 * sin(theta) * a;
    c.right = vec3(cos(yaw), 0.0, -sin(yaw));
    c.normal = vec3(sin(yaw), 0.0, cos(yaw));
    c.rect = r;
    c.offset = k;
    c.angle = theta;
    return c;
}

Camera camera(float a) {
    vec2 res = iResolution;
    Camera cam;
    cam.focal = res.y * 1.1;
    cam.eye = vec3(0.0, -res.y * p_elevation * a, cam.focal * (1.0 + 0.12 * a));
    vec3 target = vec3(0.0, res.y * 0.03 * a, 0.0);
    cam.forward = normalize(target - cam.eye);
    cam.right = normalize(cross(cam.forward, vec3(0.0, 1.0, 0.0)));
    cam.down = cross(cam.right, cam.forward);
    return cam;
}

// Where the ray meets the card's plane: distance, card-local point (y down)
// and whether it sees the front. False when it misses the plane.
bool intersect(Card c, vec3 o, vec3 d, out float t, out vec2 local, out bool front) {
    float denom = dot(d, c.normal);
    if (abs(denom) < 1e-5) return false;
    t = dot(c.center - o, c.normal) / denom;
    if (t <= 0.0) return false;
    vec3 p = o + d * t - c.center;
    local = vec2(dot(p, c.right), p.y);
    front = denom < 0.0;
    return true;
}

// A seven-segment digit in a 1 x 1.8 cell; aa is one pixel in cell units.
float digit(vec2 p, int value, float aa) {
    const int Segments[10] = int[10](0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F);
    const vec4 Boxes[7] = vec4[7](
        vec4(0.5, 0.12, 0.30, 0.09), vec4(0.84, 0.5, 0.09, 0.30), vec4(0.84, 1.3, 0.09, 0.30),
        vec4(0.5, 1.68, 0.30, 0.09), vec4(0.16, 1.3, 0.09, 0.30), vec4(0.16, 0.5, 0.09, 0.30),
        vec4(0.5, 0.9, 0.30, 0.09));
    int bits = Segments[clamp(value, 0, 9)];
    float d = 1e9;
    for (int s = 0; s < 7; ++s) {
        if ((bits & (1 << s)) != 0) d = min(d, sdRoundBox(p - Boxes[s].xy, Boxes[s].zw, 0.06));
    }
    return clamp(0.5 - d / aa, 0.0, 1.0);
}

// The card's colour at a local point, premultiplied, and its coverage.
vec4 shade(int i, Card c, vec2 local, bool front, float pixel, float a, out float coverage) {
    float corner = min(c.halfSize.x, c.halfSize.y) * 0.04 * clamp(a, 0.0, 1.0);
    float sd = sdRoundBox(local, c.halfSize, corner);
    coverage = clamp(0.5 - sd / pixel, 0.0, 1.0);
    if (coverage <= 0.0) return vec4(0.0);
    float shown = clamp(a, 0.0, 1.0);
    if (!front) {
        // The back of a card: a dark panel with a faint edge.
        float edge = smoothstep(pixel * 2.0, 0.0, -sd);
        return vec4(vec3(0.03) + uThemeCursorColor.rgb * edge * 0.25, 1.0);
    }
    vec2 cardUv = local / (2.0 * c.halfSize) + 0.5;
    // Minified cards average four taps, so small text does not shimmer.
    vec2 spread = pixel / (2.0 * c.halfSize);
    vec4 color = paneTexel(i, cardUv);
    if (spread.x * c.rect.z > 1.25) {
        color = 0.25 * (paneTexel(i, cardUv + spread * vec2(-0.25, -0.25)) + paneTexel(i, cardUv + spread * vec2(0.25, -0.25))
                      + paneTexel(i, cardUv + spread * vec2(-0.25, 0.25)) + paneTexel(i, cardUv + spread * vec2(0.25, 0.25)));
    }
    float focus = 1.0 - clamp(abs(c.offset), 0.0, 1.0);
    color.rgb *= mix(1.0, mix(p_sideBrightness, 1.0, focus), shown);

    if (i == highlightedPane()) {
        // A pulsing border in the theme's cursor colour.
        float width = c.halfSize.y * 0.018;
        float ring = smoothstep(width + pixel, width, -sd) * shown;
        float pulse = 0.8 + 0.2 * sin(iTime * 3.0);
        color = mix(color, vec4(uThemeCursorColor.rgb, 1.0), ring * pulse);
    }
    if (i < 9) {
        // The card's number, which selects it.
        vec2 unit = (local + c.halfSize) / (2.0 * c.halfSize.y);
        vec2 badge = vec2(c.halfSize.x / c.halfSize.y - 0.075, 0.075);
        float r = 0.05;
        float aa = pixel / (2.0 * c.halfSize.y);
        float disc = clamp(0.5 - (length(unit - badge) - r) / aa, 0.0, 1.0) * shown;
        float glyph = disc > 0.0 ? digit((unit - badge) / 0.032 + vec2(0.5, 0.9), i + 1, aa / 0.032) * disc : 0.0;
        color = mix(color, vec4(0.0, 0.0, 0.0, 1.0), disc * 0.7);
        color = mix(color, vec4(1.0), glyph);
    }
    return color;
}

// The window behind the cards: blurred and dimmed as the deck opens.
vec4 backdrop(vec2 uv, float a) {
    vec4 base = terminalTexel(uv);
    float radius = p_blur * a;
    if (radius < 0.5) return base;
    vec4 sum = base;
    const int Taps = 12;
    for (int k = 0; k < Taps; ++k) {
        float angle = 6.2831853 * (float(k) + 0.5) / float(Taps);
        float ring = (k % 2 == 0) ? 1.0 : 0.5;
        sum += terminalTexel(uv + terminalUv(vec2(cos(angle), sin(angle)) * radius * ring));
    }
    vec4 blurred = sum / float(Taps + 1);
    vec2 centered = uv - 0.5;
    float vignette = 1.0 - dot(centered, centered) * 0.9;
    blurred.rgb *= mix(1.0, (1.0 - p_backdropDim) * vignette, a);
    return blurred;
}

vec4 pTerminal(vec2 uv) {
    float a = overviewAmount();
    int n = min(paneCount(), 32);
    if (n == 0 || a <= 0.0005) return terminalTexel(uv);
    float shown = clamp(a, 0.0, 1.0);

    float widest = widestCard(n);
    Camera cam = camera(shown);
    vec2 q = terminalPixel(uv) - iResolution * 0.5;
    vec3 d = normalize(q.x * cam.right + q.y * cam.down + cam.focal * cam.forward);
    vec3 o = cam.eye;

    // The two nearest cards along the ray, found first and shaded after.
    Ring g = ring();
    float near = 1e20, next = 1e20;
    int nearCard = -1, nextCard = -1;
    for (int i = 0; i < n; ++i) {
        // The far side of the ring faces away: only dark backs, so skip it.
        if (shown > 0.9 && abs(ringOffset(i, g) * g.step) > 1.8) continue;
        Card c = card(i, a, widest, g);
        float t;
        vec2 local;
        bool front;
        if (!intersect(c, o, d, t, local, front)) continue;
        // One pixel at this distance, stretched where the card is seen edge-on.
        float pixel = t / cam.focal / max(abs(dot(d, c.normal)), 0.15);
        if (sdRoundBox(local, c.halfSize, 0.0) > pixel) continue;
        if (t < near) {
            next = near; nextCard = nearCard;
            near = t; nearCard = i;
        } else if (t < next) {
            next = t; nextCard = i;
        }
    }
    vec4 nearColor = vec4(0.0), nextColor = vec4(0.0);
    float nearCover = 0.0, nextCover = 0.0;
    for (int pick = 0; pick < 2; ++pick) {
        int i = pick == 0 ? nearCard : nextCard;
        if (i < 0) continue;
        Card c = card(i, a, widest, g);
        float t;
        vec2 local;
        bool front;
        intersect(c, o, d, t, local, front);
        float pixel = t / cam.focal / max(abs(dot(d, c.normal)), 0.15);
        float cover;
        vec4 color = shade(i, c, local, front, pixel, a, cover);
        if (pick == 0) { nearColor = color; nearCover = cover; }
        else { nextColor = color; nextCover = cover; }
    }

    vec4 behind = backdrop(uv, shown);
    // The floor below the cards reflects them, fading with height and distance.
    float floorY = iResolution.y * (p_cardSize * 0.5 - 0.04 + 0.012);
    if (p_reflection > 0.0 && d.y > 0.0 && o.y < floorY) {
        float tf = (floorY - o.y) / d.y;
        vec3 p = o + d * tf;
        vec3 r = vec3(d.x, -d.y, d.z);
        float best = 1e20;
        int bestCard = -1;
        for (int i = 0; i < n; ++i) {
            // Only cards near the front stand close enough to show in the floor.
            if (abs(ringOffset(i, g)) > 2.5) continue;
            Card c = card(i, a, widest, g);
            float t;
            vec2 local;
            bool front;
            if (!intersect(c, p, r, t, local, front) || t >= best) continue;
            float pixel = (tf + t) / cam.focal / max(abs(dot(r, c.normal)), 0.15);
            if (sdRoundBox(local, c.halfSize, 0.0) > pixel) continue;
            best = t;
            bestCard = i;
        }
        vec4 mirrored = vec4(0.0);
        if (bestCard >= 0) {
            Card c = card(bestCard, a, widest, g);
            float t;
            vec2 local;
            bool front;
            intersect(c, p, r, t, local, front);
            float pixel = (tf + t) / cam.focal / max(abs(dot(r, c.normal)), 0.15);
            float cover;
            vec4 color = shade(bestCard, c, local, front, pixel, a, cover);
            float height = (floorY - (p.y + r.y * t)) / (2.0 * c.halfSize.y);
            mirrored = color * cover * pow(clamp(1.0 - height * 1.6, 0.0, 1.0), 2.0);
        }
        float fade = exp(-tf / (iResolution.y * 5.0));
        behind.rgb = behind.rgb * mix(1.0, 0.75, shown) + mirrored.rgb * p_reflection * fade * shown;
    }

    vec4 under = nextColor * nextCover + behind * (1.0 - nextColor.a * nextCover);
    return nearColor * nearCover + under * (1.0 - nearColor.a * nearCover);
}
