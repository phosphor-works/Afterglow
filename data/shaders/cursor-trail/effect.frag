// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Cursor trail: after the cursor moves, a streak in the cursor's colour runs
// from the old position to the new one. Its tail catches up with an ease-out
// and the streak fades as it does. Short moves (typing) draw nothing.

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    fragColor = texture(iChannel0, fragCoord / iResolution.xy);
    if (iCursorVisible == 0 || iFocus == 0) {
        return;
    }
    float t = clamp((iTime - iTimeCursorChange) / p_duration, 0.0, 1.0);
    if (t >= 1.0) {
        return;
    }

    // Cursor rectangles are (left, top, width, height) with y up.
    vec2 halfSize = iCurrentCursor.zw * 0.5;
    vec2 head = iCurrentCursor.xy + vec2(halfSize.x, -halfSize.y);
    vec2 from = iPreviousCursor.xy + vec2(iPreviousCursor.z * 0.5, -iPreviousCursor.w * 0.5);
    if (distance(head, from) < p_minDistance * iCellSize.x) {
        return;
    }

    float ease = 1.0 - pow(1.0 - t, 3.0);
    vec2 tail = mix(from, head, ease);

    // Distance from this pixel to the cursor box swept from tail to head.
    vec2 pa = fragCoord - tail;
    vec2 ba = head - tail;
    float along = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-4), 0.0, 1.0);
    vec2 d = abs(pa - ba * along) - halfSize;
    float outside = max(d.x, d.y);
    float coverage = 1.0 - smoothstep(-0.5, 1.0, outside);

    // Brighter towards the head, gone at the tail, fading out over time.
    float alpha = coverage * mix(0.25, 1.0, along) * (1.0 - ease) * p_opacity;
    fragColor.rgb = mix(fragColor.rgb, iCurrentCursorColor.rgb, alpha);
}
