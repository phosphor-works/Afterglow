// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The panes of the window's current tab, and the pane overview, for TERMINAL
// shaders. Every pass can read the block; it serves window shaders, which see
// the whole window but not where its panes are, and the overview shader
// (Shaders.overview), which runs only while the overview shows.
//
// Panes are in layout order, the order pane rotation and the overview's
// arrow keys follow. Rectangles are in window pixels, like uTerminalRect: a
// window shader samples the point p of a pane at uv = p / iResolution.
//
// The overview opens with the pane_overview action. Afterglow eases its
// motion with Animation.overviewCurve, so shaders read positions, not ages.

#ifndef AFTERGLOW_TERMINAL_PANES_GLSL
#define AFTERGLOW_TERMINAL_PANES_GLSL

layout(std140, binding = 19) uniform TerminalPanes {
    ivec4 uPaneState;   // offset 0   .x panes (0-32), .y focused pane, .z highlighted pane (-1 for none),
                        //            .w 1 while the overview is open, 0 while closed or closing
    vec4 uOverview;     // offset 16  .x how far the overview is open: 0 the flat window, 1 open
                        //               (a spring can overshoot), .y the highlight's position: a pane
                        //               index, fractional while it moves, .z seconds since the overview
                        //               opened or closed, .w seconds since the highlight moved
                        //               (both 1e6 if never)
    vec4 uPaneRect[32]; // offset 32  each pane in window px (x, y, w, h)
};                      // total 544 bytes

int paneCount() {
    return uPaneState.x;
}

vec4 paneRect(int pane) {
    return uPaneRect[clamp(pane, 0, 31)];
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
