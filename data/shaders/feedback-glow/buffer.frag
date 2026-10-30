// SPDX-License-Identifier: GPL-3.0-or-later
// Buffer data is linear premultiplied light. Terminal input is encoded sRGB.
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec4 now = toLinearPremultiplied(texture(iChannel0, uv));
    float peak = max(now.r, max(now.g, now.b));
    now *= smoothstep(0.12, 0.55, peak);
    vec4 old = texture(iBuffer, uv) * exp(-p_decay * min(iTimeDelta, 1.0));
    fragColor = max(now, old);
}
