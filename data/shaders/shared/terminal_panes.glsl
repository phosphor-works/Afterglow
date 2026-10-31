// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The panes of the window, and the pane overview, for TERMINAL shaders.
// Every pass can read the block; it serves window shaders, which see the
// whole window but not where its panes are, and the overview shader
// (Shaders.overview), which runs only while the overview shows.
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

#ifndef AFTERGLOW_TERMINAL_PANES_GLSL
#define AFTERGLOW_TERMINAL_PANES_GLSL

layout(std140, binding = 19) uniform TerminalPanes {
    ivec4 uPaneState;      // offset 0     .x cards (0-32), .y focused card, .z highlighted card (-1 for none),
                           //              .w 1 while the overview is open, 0 while closed or closing
    vec4 uOverview;        // offset 16    .x how far the overview is open: 0 the flat window, 1 open
                           //                 (a spring can overshoot), .y the highlight's position: a card
                           //                 index, fractional while it moves, .z seconds since the overview
                           //                 opened or closed, .w seconds since the highlight moved
                           //                 (both 1e6 if never)
    ivec4 uTabState;       // offset 32    .x tabs with cards, .y the current tab's index
    vec4 uPaneRect[32];    // offset 48    each card's pane in window px (x, y, w, h)
    vec4 uPaneSource[32];  // offset 560   where its pixels are: a uv rectangle (x, y, w, h) in its source
    ivec4 uPaneTab[32];    // offset 1072  .x its tab's index, .y source: 0 the input, 1 uOverviewTab,
                           //              2 uOverviewAtlas, .z its index within its tab
};                         // total 1584 bytes

layout(binding = 20) uniform sampler2D uOverviewTab;   // the highlighted card's tab, when not current
layout(binding = 21) uniform sampler2D uOverviewAtlas; // the other tabs, smaller

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
    vec2 at = source.xy + clamp(uv, 0.0, 1.0) * source.zw;
    int kind = uPaneTab[pane].y;
    if (kind == 1) return texture(uOverviewTab, at);
    if (kind == 2) return texture(uOverviewAtlas, at);
    return texture(uTerminal, at);
}

int focusedPane() {
    return uPaneState.y;
}

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

#endif // AFTERGLOW_TERMINAL_PANES_GLSL
