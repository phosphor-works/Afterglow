// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The panes of the window, and the pane overview, for TERMINAL shaders.
// Every pass can read the block; it serves window shaders, which see the
// whole window but not where its panes are, the overview shader
// (Shaders.overview), which runs only while the overview shows, and the tab
// transition and pane effects shaders.
//
// Panes are cards in tab order, and in layout order within a tab: the order
// of pane rotation and of the overview's arrow keys. While the overview is
// closed, the cards are the current tab's panes. While it shows, they are the
// panes of every tab, up to 32. Rectangles are in window pixels, each in its
// own tab's layout.
//
// Read a card's pixels with paneTexel(): the current tab's panes come from the
// window shader's input, other tabs from images Afterglow renders while the
// overview shows. The highlighted pane's tab is at full resolution; the others
// are smaller. paneTexel() is for window and overview passes.
//
// The overview opens with the pane_overview action. Afterglow eases its
// motion with Animation.overviewCurve, so shaders read positions, not ages.
//
// A tab switch runs the tab transition shader (Shaders.tabTransition) over
// the new tab while uTabTransition.w is 1. The tab being left keeps running:
// read it with previousTabTexel(). Animation.tabCurve eases the progress.
//
// Splitting and closing panes run the pane effects shader (Shaders.paneEffects)
// while a pane opens or closes. uPaneLife says how far each card has opened and
// from which edge. A closed pane's last frame stays in uTabImage, at its old
// place, while it closes: read it with closingPaneTexel(). The progress of
// both is linear over Animation.paneEffectDuration; shaders ease it.
// A pane whose shell moves to another host, user or container (OSC 3008,
// such as afterglow ssh) changes context; the shader also runs on frames drawn
// while any pane is away from here, so it can mark those panes. Scripts play
// effects over a pane with afterglow.pulse(), read through panePulse().

#ifndef AFTERGLOW_TERMINAL_PANES_GLSL
#define AFTERGLOW_TERMINAL_PANES_GLSL

layout(std140, binding = 19) uniform TerminalPanes {
    ivec4 uPaneState;      // offset 0     .x cards (0-32), .y focused card (-1 for none), .z highlighted card
                           //              (-1 for none): while the overview closes, the card it closes onto,
                           //              .w 1 while the overview is open, 0 while closed or closing
    vec4 uOverview;        // offset 16    .x how far the overview is open: 0 the flat window, 1 open
                           //                 (a spring can overshoot), .y the highlight's position in cards:
                           //                 fractional while it moves, and counting on past the last card
                           //                 when it wraps around (take it modulo paneCount()), .z seconds since the overview
                           //                 opened or closed, .w seconds since the highlight moved
                           //                 (both 1e6 if never)
    ivec4 uTabState;       // offset 32    .x tabs with cards, .y the current tab's index
    vec4 uTabTransition;   // offset 48    .x progress: 0 the tab being left, 1 the new tab (a spring
                           //                 can overshoot), .y direction: 1 toward a tab on the
                           //                 right, -1 toward one on the left, .z seconds since it
                           //                 started, .w 1 while it runs
    vec4 uPaneRect[32];    // offset 64    each card's pane in window px (x, y, w, h)
    vec4 uPaneSource[32];  // offset 576   where its pixels are: a uv rectangle (x, y, w, h) in its source
    ivec4 uPaneTab[32];    // offset 1088  .x its tab's index, .y source: 0 the input, 1 uTabImage,
                           //              2 uTabAtlas, .z its index within its tab
    vec4 uPaneLife[32];    // offset 1600  .x how far it has opened: 0 just split, 1 open, .y the edge it
                           //              opened from: 0 none, 1 left, 2 top, 3 right, 4 bottom,
                           //              .z how far its last context change has run: 0 just changed,
                           //              1 settled, .w its context: 0 here, 1 remote, 2 elevated,
                           //              3 a container
    vec4 uClosedRect;      // offset 2112  the pane closing, in window px (x, y, w, h)
    vec4 uClosed;          // offset 2128  .x how far it has closed: 0 just closed, 1 gone, .y 1 while it shows
    vec4 uPanePulse[32];   // offset 2144  a script's afterglow.pulse: .x how far it has run: 0 just
                           //              started, 1 done, .y the effect: 0 none, 1 portal, 2 flash,
                           //              3 shake, 4-255 for packs to define, .z a palette index;
                           //              .w the pane's context before its last change (as uPaneLife.w)
};                         // total 2656 bytes

// Another tab at full resolution, laid out like the window: in the overview
// the highlighted card's tab when it is not current, in a tab transition the
// tab being left.
layout(binding = 20) uniform sampler2D uTabImage;
layout(binding = 21) uniform sampler2D uTabAtlas; // the overview's other tabs, smaller

int paneCount() {
    return uPaneState.x;
}

vec4 paneRect(int pane) {
    return uPaneRect[clamp(pane, 0, 31)];
}

int paneTab(int pane) {
    return uPaneTab[clamp(pane, 0, 31)].x;
}

int currentTab() {
    return uTabState.y;
}

bool paneInCurrentTab(int pane) {
    return paneTab(pane) == currentTab();
}

// A card's pixels at uv, from (0, 0) at its top-left to (1, 1).
vec4 paneTexel(int pane, vec2 uv) {
    pane = clamp(pane, 0, 31);
    vec4 source = uPaneSource[pane];
    int kind = uPaneTab[pane].y;
    // Stay half a texel inside the card: the sampler filters linearly, so a
    // sample on the card's edge would take in the neighbouring pane or tile.
    ivec2 size = kind == 1 ? textureSize(uTabImage, 0) : kind == 2 ? textureSize(uTabAtlas, 0) : textureSize(uTerminal, 0);
    vec2 inset = min(0.5 / max(vec2(size), vec2(1.0)), source.zw * 0.5);
    vec2 at = clamp(source.xy + clamp(uv, 0.0, 1.0) * source.zw, source.xy + inset, source.xy + source.zw - inset);
    if (kind == 1) return texture(uTabImage, at);
    if (kind == 2) return texture(uTabAtlas, at);
    return texture(uTerminal, at);
}

// -1 when no card is the focused pane.
int focusedPane() {
    return uPaneState.y;
}

// -1 when no card is highlighted. While the overview closes, the card it
// closes onto, which may be in another tab and not focused yet.
int highlightedPane() {
    return uPaneState.z;
}

bool overviewOpen() {
    return uPaneState.w != 0;
}

float overviewAmount() {
    return uOverview.x;
}

float highlightPosition() {
    return uOverview.y;
}

bool tabTransitionActive() {
    return uTabTransition.w != 0.0;
}

float tabTransitionProgress() {
    return uTabTransition.x;
}

float tabTransitionDirection() {
    return uTabTransition.y;
}

// The tab being left, at a window uv.
vec4 previousTabTexel(vec2 uv) {
    return texture(uTabImage, uv);
}

float paneOpening(int pane) {
    return uPaneLife[clamp(pane, 0, 31)].x;
}

int paneOpenEdge(int pane) {
    return int(uPaneLife[clamp(pane, 0, 31)].y + 0.5);
}

// 0 here, 1 remote, 2 elevated, 3 a container.
int paneContext(int pane) {
    return int(uPaneLife[clamp(pane, 0, 31)].w + 0.5);
}

// How far the pane's last context change has run: 0 just changed, 1 settled.
float paneContextChange(int pane) {
    return uPaneLife[clamp(pane, 0, 31)].z;
}

// The context before the last change, to blend from while it runs: a pane
// that comes back here still shows where it was.
int panePreviousContext(int pane) {
    return int(uPanePulse[clamp(pane, 0, 31)].w + 0.5);
}

// A script's pulse: how far it has run (1 when done or none).
float panePulse(int pane) {
    vec4 p = uPanePulse[clamp(pane, 0, 31)];
    return p.y > 0.5 ? p.x : 1.0;
}

int panePulseEffect(int pane) {
    return int(uPanePulse[clamp(pane, 0, 31)].y + 0.5);
}

vec3 panePulseColor(int pane) {
    return uPalette[clamp(int(uPanePulse[clamp(pane, 0, 31)].z + 0.5), 0, 255)].rgb;
}

bool closingPaneActive() {
    return uClosed.y != 0.0;
}

float closingPaneProgress() {
    return uClosed.x;
}

vec4 closingPaneRect() {
    return uClosedRect;
}

// The closed pane's last frame, at a window uv inside closingPaneRect().
vec4 closingPaneTexel(vec2 uv) {
    return texture(uTabImage, uv);
}

#endif // AFTERGLOW_TERMINAL_PANES_GLSL
