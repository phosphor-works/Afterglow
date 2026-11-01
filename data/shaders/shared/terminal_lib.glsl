// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shared helpers for TERMINAL shader packs. Pulls in the uniform contract
// (terminal_uniforms.glsl) and the colour helpers (terminal_color.glsl), and
// names the idioms packs would otherwise re-derive from raw uniforms. The host
// includes this file ahead of every pack stage (pTerminal(), pVertex() and
// pMesh()), so a pack needs no includes of its own. Buffers, audio, the
// previous text frame, mesh scenes and the window's panes are opt-in modules:
// terminal_multipass.glsl, terminal_audio.glsl, terminal_history.glsl,
// terminal_scene.glsl and terminal_panes.glsl.

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
    return uSignal[clamp(channel, 0, 7)].x;
}

float signalAge(int channel) {
    return uSignal[clamp(channel, 0, 7)].y;
}

// TIME. These read the full clock (iTimeHi + iTime), so nothing jumps when
// iTime wraps every 1024 seconds.
//
// timeAngle(rate) is the angle `rate` radians per second have turned, for
// sin() and cos() of a rotation or a wave. It equals the full clock times
// `rate` up to whole turns, and keeps full precision however long the
// window runs. Use it only through a periodic function.
float timeAngle(float rate) {
    const float tau = 6.28318530718;
    float wraps = iTimeHi / 1024.0;
    return mod(wraps * mod(1024.0 * rate, tau), tau) + iTime * rate;
}

// timeCycle(period) is the full clock's position in a cycle of `period`
// seconds, 0 to 1: the continuous fract(time / period).
float timeCycle(float period) {
    float wraps = iTimeHi / 1024.0;
    return fract(fract(wraps * fract(1024.0 / period)) + iTime / period);
}

// timeSeconds() is the full clock, for a drift that never repeats, such as a
// noise offset or a scene clock. A float loses precision as the clock grows:
// its step is about 0.5 ms after two hours and 8 ms after a day. Prefer
// timeAngle() and timeCycle() for anything periodic.
float timeSeconds() {
    return iTimeHi + iTime;
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
