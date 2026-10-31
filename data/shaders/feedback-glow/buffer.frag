// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
// Buffer data is linear premultiplied light. Terminal input is encoded sRGB.
#include <terminal_multipass.glsl>

vec4 pTerminal(vec2 uv)
{
    vec4 now = toLinearPremultiplied(terminalTexel(uv));
    float peak = max(now.r, max(now.g, now.b));
    now *= smoothstep(0.12, 0.55, peak);
    vec4 old = texture(iChannel0, uv) * exp(-p_decay * min(iTimeDelta, 1.0));
    return max(now, old);
}
