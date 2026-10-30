// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#version 440

// One instanced quad per background run, glyph, decoration or cursor part.
layout(location = 0) in vec4 rect;   // x, y, w, h in framebuffer pixels (top-left origin)
layout(location = 1) in vec4 uvRect; // atlas pixels, or shape parameters for procedural kinds
layout(location = 2) in vec4 color;  // straight RGBA
layout(location = 3) in uint kind;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
layout(location = 2) flat out uint vKind;
layout(location = 3) out vec2 vLocal;
layout(location = 4) flat out vec4 vParams;
layout(location = 5) flat out vec2 vSize;

layout(std140, binding = 0) uniform Frame {
    vec2 viewport;
    vec2 grayAtlasSize;
    vec2 colorAtlasSize;
    float time;
    float textGamma;
};

void main()
{
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vec2 pos = rect.xy + corner * rect.zw;
    // Vulkan clip space: y = -1 is the top edge.
    gl_Position = vec4(pos / viewport * 2.0 - 1.0, 0.0, 1.0);
    vUv = mix(uvRect.xy, uvRect.zw, corner);
    vColor = color;
    vKind = kind;
    vLocal = corner * rect.zw;
    vParams = uvRect;
    vSize = rect.zw;
}
