// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#version 440

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) flat in uint vKind;
layout(location = 3) in vec2 vLocal;
layout(location = 4) flat in vec4 vParams;
layout(location = 5) flat in vec2 vSize;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Frame {
    vec2 viewport;
    vec2 grayAtlasSize;
    vec2 colorAtlasSize;
    float time;
    float textGamma;
};

layout(binding = 1) uniform sampler2D grayAtlas;
layout(binding = 2) uniform sampler2D colorAtlas;

const uint KindSolid = 0u;
const uint KindGrayGlyph = 1u;
const uint KindColorGlyph = 2u;
const uint KindUndercurl = 3u;
const uint KindDotted = 4u;
const uint KindDashed = 5u;
const uint KindSubpixelGlyph = 6u;

vec4 premul(vec4 c, float coverage)
{
    float a = c.a * coverage;
    return vec4(c.rgb * a, a);
}

void main()
{
    if (vKind == KindSolid) {
        fragColor = premul(vColor, 1.0);
    } else if (vKind == KindGrayGlyph) {
        fragColor = premul(vColor, pow(texture(grayAtlas, vUv / grayAtlasSize).r, 1.0 / textGamma));
    } else if (vKind == KindSubpixelGlyph) {
        vec3 coverage = pow(texture(colorAtlas, vUv / colorAtlasSize).rgb, vec3(1.0 / textGamma));
        fragColor = vec4(coverage * vColor.a, 0.0);
    } else if (vKind == KindColorGlyph) {
        fragColor = texture(colorAtlas, vUv / colorAtlasSize) * vColor.a;
    } else if (vKind == KindUndercurl) {
        // vParams.x: line thickness, vParams.y: wavelength. Phase follows the
        // absolute x so adjacent runs join seamlessly.
        float thickness = vParams.x;
        float amplitude = max(0.0, (vSize.y - thickness) * 0.5);
        float centre = vSize.y * 0.5;
        float y = centre + amplitude * sin(gl_FragCoord.x * 6.2831853 / vParams.y);
        float d = abs(vLocal.y - y);
        fragColor = premul(vColor, clamp(thickness * 0.5 + 0.5 - d, 0.0, 1.0));
    } else {
        // Dotted and dashed: vParams.x is the length of each on and off segment.
        float on = step(mod(gl_FragCoord.x, vParams.x * 2.0), vParams.x);
        fragColor = premul(vColor, on);
    }
}
