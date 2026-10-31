// SPDX-License-Identifier: GPL-3.0-or-later
vec4 pVertex(vec2 uv) {
    // Overscan subtly pulses without exposing an uncovered border.
    vec4 position = terminalQuadPosition(uv);
    position.xy *= 1.01 + 0.005 * sin(iTime);
    return position;
}
