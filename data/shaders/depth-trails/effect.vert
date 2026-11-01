// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
vec4 pVertex(vec2 uv) {
    // An oversized quad that pulses. The final pass computes uv from the pixel
    // position, so the quad's size changes which pixels run, not where the
    // content lands; the quad always covers the whole canvas.
    vec4 position = terminalQuadPosition(uv);
    position.xy *= 1.01 + 0.005 * sin(timeAngle(1.0));
    return position;
}
