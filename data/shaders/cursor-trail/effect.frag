// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Cursor trail: after the cursor moves, a streak in the cursor's colour runs
// from the old position to the new one. Its tail catches up with an ease-out
// and the streak fades as it does. Short moves (typing) draw nothing.

vec4 pTerminal(vec2 uv)
{
    vec4 color = terminalTexel(uv);
    if (!cursorVisible() || !paneFocused()) {
        return color;
    }
    float t = clamp(cursorAge() / p_duration, 0.0, 1.0);
    if (t >= 1.0) {
        return color;
    }

    vec2 halfSize = uCursorRect.zw * 0.5;
    vec2 head = cursorCenter();
    vec2 from = previousCursorCenter();
    if (distance(head, from) < p_minDistance * uCellSize.x) {
        return color;
    }

    float ease = 1.0 - pow(1.0 - t, 3.0);
    vec2 tail = mix(from, head, ease);

    // Distance from this pixel to the cursor box swept from tail to head.
    vec2 pa = terminalPixel(uv) - tail;
    vec2 ba = head - tail;
    float along = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-4), 0.0, 1.0);
    vec2 d = abs(pa - ba * along) - halfSize;
    float outside = max(d.x, d.y);
    float coverage = 1.0 - smoothstep(-0.5, 1.0, outside);

    // Brighter towards the head, gone at the tail, fading out over time.
    float alpha = coverage * mix(0.25, 1.0, along) * (1.0 - ease) * p_opacity;
    color.rgb = mix(color.rgb, uCursorColor.rgb, alpha);
    return color;
}
