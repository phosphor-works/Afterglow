// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "phosphor.glsl"

vec4 pTerminal(vec2 uv) {
    vec4 c = terminalTexel(uv);
    if (!paneFocused() || !cursorVisible()) return c;
    vec2 pixel = terminalPixel(uv);
    vec2 at = cursorCenter();
    vec2 previous = previousCursorCenter();
    vec2 d = at - previous;
    float along = clamp(dot(pixel - previous, d) / max(dot(d, d), 1.0), 0.0, 1.0);
    float distance = length(pixel - mix(previous, at, along));
    float age = cursorAge();
    float wake = exp(-phSq(distance / max(1.0, uCellSize.y * 0.12)))
               * exp(-age * 9.0) * along * smoothstep(2.0, 10.0, length(d));
    vec3 tint = mix(phViolet, phCyan, along);
    float alpha = wake * p_wake * (1.0 - c.a);
    c += vec4((phLight() ? tint * 0.4 : tint) * alpha, alpha);
    return clamp(c, 0.0, 1.0);
}
