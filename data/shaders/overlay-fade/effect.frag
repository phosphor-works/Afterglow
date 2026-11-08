// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay Fade: the overlay transition. This pass runs over the pane of an
// overlay while it fades in or out. Its input is the overlay, and the pane
// under it comes from overlayHostTexel(). Both have their pane shaders. At
// progress 0 it is the pane alone, at 1 the overlay alone. Afterglow keeps
// the progress between 0 and 1, so a curve that overshoots needs no clamp.

#include <terminal_panes.glsl>

vec4 pTerminal(vec2 uv) {
    // Premultiplied colours blend linearly: a translucent pane stays as translucent.
    return mix(overlayHostTexel(uv), terminalTexel(uv), overlayTransitionProgress());
}
