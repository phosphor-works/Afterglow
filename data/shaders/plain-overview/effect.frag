// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Plain Overview: the flat overview of flat_overview.glsl with no gradient
// and no halo. The rim of the highlighted pane takes the theme's cursor
// colour.

#define OVERVIEW_TINT(t) uThemeCursorColor.rgb

#include <flat_overview.glsl>

vec4 pTerminal(vec2 uv) {
    return flatOverview(uv);
}
