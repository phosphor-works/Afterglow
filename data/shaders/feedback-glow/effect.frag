// SPDX-License-Identifier: GPL-3.0-or-later
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec4 inputColor = texture(iChannel0, uv);
    vec4 light = texture(iBuffer, uv);
    // Current text remains intact; retained light is deliberately subtle.
    vec4 linear = toLinearPremultiplied(inputColor);
    float alpha = max(linear.a, light.a * p_strength);
    fragColor = toSrgbPremultiplied(vec4(min(linear.rgb + light.rgb * p_strength, vec3(alpha)), alpha));
}
