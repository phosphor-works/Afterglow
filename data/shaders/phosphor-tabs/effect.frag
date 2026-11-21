// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Phosphor Tabs: the tab transition, in the Phosphor design language.
// flat_tabs.glsl has the motion: the new tab slides in flat over the tab
// being left, or the two fade. This pack gives the edge between them a line
// of the spectrum, bright at once and gone as the new tab arrives.

#include <phosphor_spectrum.glsl>

#define TAB_SEAM_TINT(t) spectrum(t)

#include <flat_tabs.glsl>

vec4 pTerminal(vec2 uv) {
    return flatTabs(uv);
}
