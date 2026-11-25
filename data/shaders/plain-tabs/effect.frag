// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Plain Tabs: the flat tab transition of flat_tabs.glsl as it is, with no
// line at the edge between the tabs and no colour of its own.

#include <flat_tabs.glsl>

vec4 pTerminal(vec2 uv) {
    return flatTabs(uv);
}
