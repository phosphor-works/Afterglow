// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Plain Panes: the flat pane effects of flat_panes.glsl with no rim and no
// glow. The line of a pane that is away from here takes a palette colour of
// the theme: bright cyan for a remote host, bright red for an elevated user,
// bright magenta for a container.

#define PANE_CONTEXT_TINT(context) paletteColor((context) == 2 ? 9 : (context) == 3 ? 13 : 14)

#include <flat_panes.glsl>

vec4 pTerminal(vec2 uv) {
    return flatPanes(uv);
}
