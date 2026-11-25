// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Phosphor Panes: panes opening from a split and closing, in the Phosphor
// design language. flat_panes.glsl has the motion: panes fade in and out,
// and a pane whose shell is away from here keeps a line at its edge. This
// pack gives it the spectrum: the rim of a pane that opens or closes carries
// the gradient, bright at once and then fading, and the line of a pane that
// is away is cyan for a remote host, rose for an elevated user and purple
// for a container, bright at the change before it settles.

#include <phosphor_spectrum.glsl>

#define PANE_EDGE_TINT(pixel, r) spectrum(((pixel).x - (r).x) / max((r).z, 1.0))
#define PANE_CONTEXT_TINT(context) spectrumContext(context)
#define PANE_RIM p_rim
#define PANE_GLOW p_glow
#define PANE_REACH p_reach
#define PANE_HALO 1.0
#define PANE_SETTLED 0.7

#include <flat_panes.glsl>

vec4 pTerminal(vec2 uv) {
    return flatPanes(uv);
}
