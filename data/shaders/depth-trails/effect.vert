// SPDX-License-Identifier: GPL-3.0-or-later
void mainVertex(inout vec4 position, in vec2 uv) {
    // Overscan subtly pulses without exposing an uncovered border.
    position.xy *= 1.01 + 0.005 * sin(iTime);
}
