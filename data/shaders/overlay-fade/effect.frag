// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay Fade: the overlay transition. This pass runs over the pane of an
// overlay while it fades in or out. Its input is the overlay, and the pane
// under it comes from overlayHostTexel(). Both have their pane shaders. At
// progress 0 it is the pane alone, at 1 the overlay alone.

#include <terminal_panes.glsl>

vec4 pTerminal(vec2 uv) {
    float progress = clamp(overlayTransitionProgress(), 0.0, 1.0);
    // Premultiplied colours blend linearly: a translucent pane stays as translucent.
    return mix(overlayHostTexel(uv), terminalTexel(uv), progress);
}
