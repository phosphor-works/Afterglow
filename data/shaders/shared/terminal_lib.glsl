// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shared helpers for TERMINAL shader packs. Pulls in the uniform contract
// (terminal_uniforms.glsl) and the colour helpers (terminal_color.glsl), and
// names the idioms packs would otherwise re-derive from raw uniforms. The host
// includes this file ahead of every pack that defines pTerminal(), so such a
// pack needs no includes of its own. Buffers, audio and the previous text frame
// are opt-in modules: terminal_multipass.glsl, terminal_audio.glsl and
// terminal_history.glsl.

#ifndef AFTERGLOW_TERMINAL_LIB_GLSL
#define AFTERGLOW_TERMINAL_LIB_GLSL

#include <terminal_uniforms.glsl>
#include <terminal_color.glsl>

// A canvas coordinate in canvas pixels, and back.
vec2 terminalPixel(vec2 uv) {
    return uv * iResolution;
}

vec2 terminalUv(vec2 pixel) {
    return pixel / iResolution;
}

// The canvas's own content at `uv`.
vec4 terminalTexel(vec2 uv) {
    return texture(uTerminal, uv);
}

bool cursorVisible() {
    return uCursorState.z != 0;
}

bool paneFocused() {
    return uCursorState.w != 0;
}

vec2 cursorCenter() {
    return uCursorRect.xy + uCursorRect.zw * 0.5;
}

vec2 previousCursorCenter() {
    return uPreviousCursorRect.xy + uPreviousCursorRect.zw * 0.5;
}

float cursorAge() {
    return uEventAges.x;
}

float focusAge() {
    return uEventAges.y;
}

float signalValue(int channel) {
    return uSignal[channel].x;
}

float signalAge(int channel) {
    return uSignal[channel].y;
}

vec3 paletteColor(int index) {
    return uPalette[clamp(index, 0, 255)].rgb;
}

// For a pack's vertex stage (metadata `vertexShader`): the undeformed canvas
// quad's clip position at `uv`. pVertex(uv) returns this to draw the canvas
// as is, and the host then places the canvas in its render target.
vec4 terminalQuadPosition(vec2 uv) {
    return vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}

#endif // AFTERGLOW_TERMINAL_LIB_GLSL
