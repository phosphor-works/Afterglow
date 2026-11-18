// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Phosphor Overview: the pane overview, in the Phosphor design language.
// flat_overview.glsl has the layout: each tab is a small picture of its
// panes, each pane in its real place. This pack gives the highlighted pane
// the spectrum on its rim, with a halo behind it.

#include <phosphor_spectrum.glsl>

#define OVERVIEW_TINT(t) spectrum(t)
#define OVERVIEW_GLOW p_glow
#define OVERVIEW_REACH p_reach

#include <flat_overview.glsl>

vec4 pTerminal(vec2 uv) {
    return flatOverview(uv);
}
