// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#version 440

// Kitty graphics images: straight-alpha RGBA, drawn with grid.vert.
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
// Declared to match grid.vert's outputs; unused here.
layout(location = 2) flat in uint vKind;
layout(location = 3) in vec2 vLocal;
layout(location = 4) flat in vec4 vParams;
layout(location = 5) flat in vec2 vSize;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D image;

void main()
{
    vec4 c = texture(image, vUv);
    float a = c.a * vColor.a;
    fragColor = vec4(c.rgb * a, a);
}
