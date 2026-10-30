// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#version 440

// Puts the background, text and cursor layers (premultiplied) back together.

layout(std140, binding = 0) uniform Composite {
    vec2 framebufferSize;
    int cursorBelowText; // a block cursor sits under its text
    int pad;
};

layout(binding = 1) uniform sampler2D backgroundLayer;
layout(binding = 2) uniform sampler2D textLayer;
layout(binding = 3) uniform sampler2D cursorLayer;
layout(binding = 4) uniform sampler2D otherCursorLayer;

layout(location = 0) out vec4 fragColor;

vec4 over(vec4 top, vec4 bottom)
{
    return top + bottom * (1.0 - top.a);
}

void main()
{
    vec2 uv = gl_FragCoord.xy / framebufferSize;
    vec4 background = texture(backgroundLayer, uv);
    vec4 text = texture(textLayer, uv);
    vec4 cursor = texture(cursorLayer, uv);
    vec4 other = texture(otherCursorLayer, uv);
    fragColor = cursorBelowText != 0 ? over(other, over(text, over(cursor, background)))
                                    : over(cursor, over(text, over(other, background)));
}
