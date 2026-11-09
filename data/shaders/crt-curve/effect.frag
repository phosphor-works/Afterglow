// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// CRT Curve: the canvas bends like the glass of an old tube, with darker
// corners. pPointerMap gives the mouse the same bend, so a click lands on the
// cell drawn under the pointer, not on the cell that the layout has there.

// Where the content shown at uv comes from: a barrel bend around the centre,
// stronger towards the edges. Content near the corners moves out of the canvas,
// which leaves the corners dark.
vec2 crtWarp(vec2 uv)
{
    vec2 c = uv * 2.0 - 1.0;
    c *= 1.0 + p_curvature * dot(c, c);
    return c * 0.5 + 0.5;
}

vec4 pTerminal(vec2 uv)
{
    vec2 source = crtWarp(uv);
    if (any(lessThan(source, vec2(0.0))) || any(greaterThan(source, vec2(1.0)))) return vec4(0.0, 0.0, 0.0, 1.0);
    vec4 color = terminalTexel(source);
    // Darker towards the edges of the glass.
    vec2 c = uv * 2.0 - 1.0;
    float shade = 1.0 - p_vignette * dot(c, c) * 0.5;
    return vec4(color.rgb * shade, color.a);
}

// The pointer map: the same bend as the picture. It reads only uv and the
// parameters, as the contract requires.
vec2 pPointerMap(vec2 uv)
{
    return crtWarp(uv);
}
