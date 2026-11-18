// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Phosphor Overview: the pane overview, in the Phosphor design language. The
// window steps back, and each tab becomes a small picture of its layout, with
// every pane in its real place and its live pixels. The tabs sit in a grid,
// in tab order. Nothing turns: every card is a flat rectangle, so a pixel
// finds its cards with rectangle tests and no rays.
//
// The highlighted pane lifts a little and its rim carries the spectrum, with
// a halo behind it. The highlight follows highlightPosition(), which
// Afterglow eases, so the rim fades from one pane to the next.
//
// At overviewAmount() 0 every card is exactly where its pane is, with other
// tabs one window width per tab to the side, so opening and closing start
// and end on the window. The pack reads no clock.

#include <terminal_panes.glsl>
#include <phosphor_spectrum.glsl>

// The grid of tabs: the same for every card, so computed once a pixel.
struct Layout {
    int tabs;
    int columns;
    int rows;
    vec2 cell;   // one tab's place, in window px
    float scale; // a tab's picture against the window
};

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

Layout tabGrid(int n) {
    vec2 res = iResolution;
    Layout g;
    g.tabs = max(paneTab(n - 1) - paneTab(0) + 1, 1);
    // The smallest square grid that holds the tabs, in whole numbers: a
    // sqrt() that is a little high would add a column at 4 or 9 tabs.
    g.columns = 1;
    while (g.columns * g.columns < g.tabs) ++g.columns;
    g.rows = (g.tabs + g.columns - 1) / g.columns;
    g.cell = res * (1.0 - 2.0 * p_margin) / vec2(float(g.columns), float(g.rows));
    g.scale = min(g.cell.x / res.x, g.cell.y / res.y) * (g.tabs > 1 ? 1.0 - p_tabGap : 1.0);
    return g;
}

// The highlight: the card it is on, or the two cards it moves between, and
// how close it is to each, 0 to 1. Every other card has a focus of 0, so a
// pixel works these out once and not for each card.
struct Highlight {
    int from;
    int to; // -1 when the highlight is on one card
    float toAmount;
};

Highlight highlight(int n) {
    Highlight h;
    h.to = -1;
    h.toAmount = 0.0;
    // With no highlighted card, the focused pane has it.
    h.from = focusedPane();
    if (highlightedPane() < 0 || n < 2) return h;
    // The highlight keeps counting past the last card when it wraps around.
    float position = mod(highlightPosition(), float(n));
    h.from = min(int(floor(position)), n - 1);
    h.toAmount = clamp(position - float(h.from), 0.0, 1.0);
    if (h.toAmount > 0.0) h.to = (h.from + 1) % n;
    return h;
}

float focusOf(int i, Highlight h) {
    return i == h.from ? 1.0 - h.toAmount : i == h.to ? h.toAmount : 0.0;
}

// Card i's rectangle in window px (x, y, width, height), with the overview
// open by a.
vec4 cardRect(int i, Layout g, float a, float focus) {
    vec2 res = iResolution;
    vec4 r = paneRect(i);
    // Flat: where the pane is in its tab, with other tabs to the side.
    vec4 inWindow = vec4(r.x + float(paneTab(i) - currentTab()) * res.x, r.yzw);
    // In the overview: the pane's place in its tab's picture. The last row
    // of tabs is centred when it is not full.
    int tab = paneTab(i) - paneTab(0);
    int row = tab / g.columns;
    int inRow = row == g.rows - 1 ? g.tabs - row * g.columns : g.columns;
    vec2 slot = vec2(float(tab % g.columns) - float(inRow - 1) * 0.5, float(row) - float(g.rows - 1) * 0.5);
    vec2 center = res * 0.5 + slot * g.cell + (r.xy + r.zw * 0.5 - res * 0.5) * g.scale;
    // The highlighted card grows into the gap round it, and never over the
    // next card.
    vec2 size = max(r.zw * g.scale - p_gap, vec2(1.0));
    size += min(size * p_lift, vec2(p_gap)) * focus;
    // A spring can carry a past 1: the size stays above zero.
    vec2 centerNow = mix(inWindow.xy + inWindow.zw * 0.5, center, a);
    vec2 sizeNow = max(mix(inWindow.zw, size, a), vec2(1.0));
    return vec4(centerNow - sizeNow * 0.5, sizeNow);
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

// The card's colour at a window pixel inside it. sd is the pixel's distance
// from the card's edge, negative inside.
vec4 shade(int i, vec4 rect, vec2 pixel, float sd, float focus, float shown) {
    vec2 cardUv = (pixel - rect.xy) / rect.zw;
    // A card smaller than its pane averages the pane's pixels under each of
    // its own, so small text does not shimmer: four taps, or sixteen below a
    // third of the size. The atlas of the other tabs is small already.
    float shrink = paneRect(i).z / rect.z;
    vec2 texel = 1.0 / rect.zw;
    vec4 color;
    if (shrink > 3.0 && uPaneTab[clamp(i, 0, 31)].y != 2) {
        color = vec4(0.0);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) color += paneTexel(i, cardUv + texel * ((vec2(float(x), float(y)) + 0.5) / 4.0 - 0.5));
        }
        color /= 16.0;
    } else if (shrink > 1.25) {
        vec2 spread = 0.25 * texel;
        color = 0.25 * (paneTexel(i, cardUv + spread * vec2(-1.0, -1.0)) + paneTexel(i, cardUv + spread * vec2(1.0, -1.0))
                      + paneTexel(i, cardUv + spread * vec2(-1.0, 1.0)) + paneTexel(i, cardUv + spread * vec2(1.0, 1.0)));
    } else {
        color = paneTexel(i, cardUv);
    }
    color.rgb *= mix(1.0, mix(p_otherBrightness, 1.0, focus), shown);

    if (p_numbers > 0.5 && i < 9) {
        // The card's number, which selects it: a dark chip in the top corner.
        float size = clamp(rect.w * 0.09, 14.0, 30.0);
        vec2 chip = vec2(rect.x + rect.z - size * 0.9, rect.y + size * 0.9);
        float disc = clamp(0.5 - (sdRoundBox(pixel - chip, vec2(size * 0.5), size * 0.22)), 0.0, 1.0) * shown;
        if (disc > 0.0) {
            float cell = size * 0.42;
            float glyph = digit((pixel - chip) / cell + vec2(0.5, 0.9), i + 1, 1.0 / cell);
            color = mix(color, vec4(vec3(0.0), 1.0), disc * 0.72);
            color = mix(color, vec4(mix(uForegroundColor.rgb, vec3(1.0), 0.5), 1.0), glyph * disc);
        }
    }

    // The edge: a faint line on every card, and the spectrum on the
    // highlighted one.
    float inside = -sd;
    float line = 1.0 - smoothstep(0.5, 1.5, inside);
    color = mix(color, vec4(uForegroundColor.rgb, 1.0), line * p_outline * (1.0 - focus) * shown);
    vec3 tint = spectrum((pixel.x - rect.x) / max(rect.z, 1.0));
    color = coverWith(color, tint, (1.0 - smoothstep(p_rimWidth - 0.5, p_rimWidth + 0.5, inside)) * p_rim * focus * shown);
    return color;
}

// Card i's distance from a window pixel, negative inside the card, and its
// rectangle. False when the pixel is more than `reach` pixels outside it.
bool cardAt(int i, Layout g, float a, float focus, float shown, vec2 pixel, float reach, out vec4 rect, out float sd) {
    rect = cardRect(i, g, a, focus);
    vec2 halfSize = rect.zw * 0.5;
    vec2 local = pixel - rect.xy - halfSize;
    if (any(greaterThan(abs(local), halfSize + reach))) return false;
    sd = sdRoundBox(local, halfSize, min(p_corner * shown, min(halfSize.x, halfSize.y)));
    return true;
}

vec4 pTerminal(vec2 uv) {
    float a = overviewAmount();
    int n = min(paneCount(), 32);
    if (n == 0 || a <= 0.0005) return terminalTexel(uv);
    float shown = clamp(a, 0.0, 1.0);
    vec2 pixel = terminalPixel(uv);
    Layout g = tabGrid(n);

    // The window fades to a dark backdrop behind the cards.
    vec4 deep = vec4(uBackgroundColor.rgb * (1.0 - p_backdropDim), 1.0) * uBackgroundColor.a;
    vec4 color = mix(terminalTexel(uv), deep, shown);

    // A card covers what is behind it. Cards never overlap when the overview
    // is open, and cross only while it opens. The highlighted card, and the
    // one that the highlight moves to, go over the others, with their halo
    // between.
    Highlight h = highlight(n);
    vec3 halo = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        vec4 rect;
        float sd;
        if (i == h.from || i == h.to) {
            float focus = focusOf(i, h);
            if (!cardAt(i, g, a, focus, shown, pixel, p_reach * 6.0, rect, sd)) continue;
            vec3 tint = spectrum((pixel.x - rect.x) / max(rect.z, 1.0));
            halo += tint * exp(-max(sd, 0.0) / p_reach) * p_glow * focus * shown;
            continue;
        }
        if (!cardAt(i, g, a, 0.0, shown, pixel, 1.0, rect, sd)) continue;
        float coverage = clamp(0.5 - sd, 0.0, 1.0);
        if (coverage > 0.0) color = mix(color, shade(i, rect, pixel, sd, 0.0, shown), coverage);
    }
    color = addLight(color, halo * max(color.a, 0.5));
    for (int k = 0; k < 2; ++k) {
        int i = k == 0 ? h.to : h.from;
        if (i < 0 || i >= n) continue;
        float focus = focusOf(i, h);
        vec4 rect;
        float sd;
        if (!cardAt(i, g, a, focus, shown, pixel, 1.0, rect, sd)) continue;
        float coverage = clamp(0.5 - sd, 0.0, 1.0);
        if (coverage > 0.0) color = mix(color, shade(i, rect, pixel, sd, focus, shown), coverage);
    }
    return color;
}
