// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include <terminal_multipass.glsl>

vec4 pTerminal(vec2 uv)
{
    vec4 light = texture(iChannel0, uv);
    // Current text remains intact; retained light is deliberately subtle.
    vec4 linear = toLinearPremultiplied(terminalTexel(uv));
    float alpha = max(linear.a, light.a * p_strength);
    return toSrgbPremultiplied(vec4(min(linear.rgb + light.rgb * p_strength, vec3(alpha)), alpha));
}
