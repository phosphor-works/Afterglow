// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Phosphor Depths buffer pass: the deep glow, in linear light, for effect.frag.

#include "depths.glsl"

vec4 pTerminal(vec2 uv)
{
    return vec4(deepGlow(uv), 1.0);
}
