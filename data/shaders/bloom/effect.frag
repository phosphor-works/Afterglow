// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Bloom: gathers bright pixels from three rings around each pixel and adds
// them back as a soft glow. Inner rings weigh more than outer ones.

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec4 base = texture(iChannel0, uv);

    const int Rings = 3;
    const int Taps = 10;
    vec3 glow = vec3(0.0);
    float total = 0.0;
    for (int r = 1; r <= Rings; ++r) {
        float radius = p_radius * float(r) / float(Rings);
        float weight = 1.0 / float(r);
        for (int i = 0; i < Taps; ++i) {
            // Offset each ring's angle so taps do not line up across rings.
            float a = 6.2831853 * (float(i) + 0.5 * float(r)) / float(Taps);
            vec2 offset = vec2(cos(a), sin(a)) * radius / iResolution.xy;
            vec3 s = texture(iChannel0, uv + offset).rgb;
            float luma = dot(s, vec3(0.2126, 0.7152, 0.0722));
            glow += s * smoothstep(p_threshold, 1.0, luma) * weight;
            total += weight;
        }
    }
    fragColor = vec4(base.rgb + glow / total * p_strength, base.a);
}
