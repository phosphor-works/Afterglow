// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Canonical uniform contract for TERMINAL shader packs, Afterglow's shader
// family alongside Phosphor's animation, overlay, pointer and surface families.
// A terminal shader shades one canvas: a pane's background, text or cursor
// layer, a whole pane, the whole window, or one of the pack's buffer passes.
//
// The block opens with Phosphor's base region (PhosphorShaders::BaseUniforms,
// 672 bytes, byte for byte) and continues with the terminal tail. The C++
// mirror is AfterglowRender::TerminalUniforms, whose static_asserts pin every
// offset below.
//
// COORDINATES. Everything is top-down: y grows downwards in every position,
// rectangle and texture coordinate, and no input needs flipping. `uv` is the
// canvas from (0, 0) at its top-left to (1, 1) at its bottom-right, and
// terminalPixel(uv) is the same point in canvas pixels. Canvas pixels are
// device pixels, scaled down in a buffer pass with a bufferScale below 1.
//
// TIME. iTime wraps every 1024 seconds, and iTimeHi holds the seconds the
// wraps removed, so iTimeHi + iTime is the full clock. sin(iTime * rate) and
// fract(iTime / period) jump at each wrap unless the rate or the period fits
// 1024 seconds a whole number of times. Read time through terminal_lib.glsl's
// timeAngle(), timeCycle() and timeSeconds(), which stay continuous. Events
// are given as ages in seconds rather than as timestamps, so they need no
// clock arithmetic.

#ifndef AFTERGLOW_TERMINAL_UNIFORMS_GLSL
#define AFTERGLOW_TERMINAL_UNIFORMS_GLSL

layout(std140, binding = 0) uniform TerminalUniforms {
    // ── base region (PhosphorShaders::BaseUniforms) ──────────────────────────
    mat4 qt_Matrix;              // offset 0    identity, unused here
    float qt_Opacity;            // offset 64   always 1
    float iTime;                 // offset 68   seconds, wrapped every 1024 s
    float iTimeDelta;            // offset 72   seconds since the previous frame
    int iFrame;                  // offset 76   frame counter; in a buffer pass, 0 after a buffer reset
    vec2 iResolution;            // offset 80   canvas size, canvas px
    int _appField0;              // offset 88   unused
    int _appField1;              // offset 92   unused
    vec4 iMouse;                 // offset 96   pointer: .xy canvas px, .zw normalized;
                                 //             all -1 when the pointer is off the canvas
    vec4 iDate;                  // offset 112  year, month (1-12), day, seconds since midnight
    vec4 customParams[8];        // offset 128  scalar parameters, read through p_<id>
    vec4 customColors[16];       // offset 256  colour parameters, read through p_<id>
    vec4 iChannelResolution[4];  // offset 512  sizes (.xy) of iChannel0..3
    int iAudioSpectrumSize;      // offset 576  spectrum bars, 0 without audio
    int iFlipBufferY;            // offset 580  always 0: buffers are top-down too
    vec4 iTextureResolution[4];  // offset 592  sizes (.xy) of the image parameters uTexture0..3
    float iTimeHi;               // offset 656  whole wraps of iTime, in seconds
    int iIsReversed;             // offset 660  always 0
    // ── terminal tail (AfterglowRender::TerminalUniformsTail) ────────────────
    vec4 uCanvas;                // offset 672  the canvas in its render target, px (x, y, w, h)
    vec2 uTargetSize;            // offset 688  the render target, px
    vec2 uCellSize;              // offset 696  one character cell, canvas px
    vec4 uTerminalRect;          // offset 704  what this pass shades, in window px (x, y, w, h);
                                 //             not scaled in buffer passes
    vec2 uWindowSize;            // offset 720  the window, px
    vec2 uWindowMouse;           // offset 728  pointer in window px, for scenes that span panes;
                                 //             -1 when it is outside the window
    vec4 uCursorRect;            // offset 736  the cursor, canvas px (x, y, w, h)
    vec4 uPreviousCursorRect;    // offset 752  where it was before it last moved
    vec4 uCursorColor;           // offset 768  the cursor as drawn
    vec4 uPreviousCursorColor;   // offset 784
    ivec4 uCursorState;          // offset 800  .x shape, .y previous shape (kCursor*;
                                 //             kCursorBlockHollow in an unfocused pane,
                                 //             whatever the shape), .z visible, .w pane focused
    vec4 uEventAges;             // offset 816  seconds since: .x the cursor moved, .y focus changed;
                                 //             1e6 means never: a pane seen for the first time, and every
                                 //             pane on the first frame after frames without shaders.
                                 //             Only a move of the cursor rectangle sets .x
    vec4 uBackgroundColor;       // offset 832  theme background; .a is the window's background opacity
    vec4 uForegroundColor;       // offset 848  theme foreground
    vec4 uThemeCursorColor;      // offset 864  theme cursor colour
    vec4 uThemeCursorTextColor;  // offset 880  theme colour of text under the cursor
    vec4 uSelectionBackgroundColor; // offset 896
    vec4 uSelectionForegroundColor; // offset 912
    vec4 uAudioLevels;           // offset 928  bass, mids, treble, overall, 0..1; see terminal_audio.glsl
    vec4 uScrollback;            // offset 944  history rows: .x pane, .y tab, .z tab eased, .w 0
    vec4 uSignal[8];             // offset 960  script signals: .x value, .y seconds since set
                                 //             (1e6 when never set)
    vec4 uPalette[256];          // offset 1088 the 256-colour palette
};                               // total 5184 bytes

// The canvas being shaded: a layer, the pane or the window, in premultiplied
// encoded sRGB. Its texture coordinates are the canvas's `uv`.
layout(binding = 1) uniform sampler2D uTerminal;

// Image parameters, declared in metadata with type "image"; p_<id> names them.
layout(binding = 11) uniform sampler2D uTexture0;
layout(binding = 12) uniform sampler2D uTexture1;
layout(binding = 13) uniform sampler2D uTexture2;
layout(binding = 14) uniform sampler2D uTexture3;

const int kCursorBlock = 0;
const int kCursorBlockHollow = 1;
const int kCursorBar = 2;
const int kCursorUnderline = 3;

#endif // AFTERGLOW_TERMINAL_UNIFORMS_GLSL
