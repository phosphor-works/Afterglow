// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// CRT Curve: the canvas bends like the glass of an old tube, with darker
// corners. pPointerMap gives the mouse the same bend, so a click lands on the
// cell drawn under the pointer, not on the cell that the layout has there.

// Where the content shown at uv comes from: a barrel bend around the centre,
// stronger towards the edges. The content moves in towards the centre, so the
// canvas near its edges, most at the corners, shows nothing and stays dark.
vec2 crtWarp(vec2 uv)
{
    vec2 c = uv * 2.0 - 1.0;
    c *= 1.0 + p_curvature * dot(c, c);
    return c * 0.5 + 0.5;
}

vec4 pTerminal(vec2 uv)
{
    vec2 source = crtWarp(uv);
    // Past the glass is the dark of the tube, black at the window's opacity so
    // that a translucent window stays translucent there. The theme background
    // would hide the bend wherever the screen shows only background.
    vec4 tube = vec4(0.0, 0.0, 0.0, uBackgroundColor.a);
    // How much of this pixel the glass covers: the distance to its edge in
    // pixels, blended over one pixel so that the bent edge is not jagged.
    vec2 edge = min(source, 1.0 - source) / max(fwidth(source), vec2(1e-6));
    vec2 cover = clamp(edge + 0.5, 0.0, 1.0);
    float coverage = cover.x * cover.y;
    if (coverage <= 0.0) return tube;
    vec4 color = terminalTexel(source);
    // Darker towards the edges of the glass.
    vec2 c = uv * 2.0 - 1.0;
    float shade = 1.0 - p_vignette * dot(c, c) * 0.5;
    return mix(tube, vec4(color.rgb * shade, color.a), coverage);
}

// The pointer map: the same bend as the picture. It reads only uv and the
// parameters, as the contract requires.
vec2 pPointerMap(vec2 uv)
{
    return crtWarp(uv);
}
