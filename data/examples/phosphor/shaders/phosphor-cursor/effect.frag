// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "phosphor.glsl"

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec4 c = texture(iChannel0, fragCoord / iResolution.xy);
    if (iFocus == 0 || iCursorVisible == 0) { fragColor = c; return; }
    vec2 at = phCursor();
    vec2 previous = iPreviousCursor.xy + vec2(iPreviousCursor.z, -iPreviousCursor.w) * 0.5;
    vec2 d = at - previous;
    float along = clamp(dot(fragCoord - previous, d) / max(dot(d, d), 1.0), 0.0, 1.0);
    float distance = length(fragCoord - mix(previous, at, along));
    float age = max(0.0, iTime - iTimeCursorChange);
    float wake = exp(-phSq(distance / max(1.0, iCellSize.y * 0.12)))
               * exp(-age * 9.0) * along * smoothstep(2.0, 10.0, length(d));
    vec3 tint = mix(phViolet, phCyan, along);
    float alpha = wake * p_wake * (1.0 - c.a);
    c += vec4((phLight() ? tint * 0.4 : tint) * alpha, alpha);
    fragColor = clamp(c, 0.0, 1.0);
}
