// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "phosphor.glsl"

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec4 text = phText(fragCoord);
    vec4 previous = texture(iPreviousText, fragCoord / iResolution.xy);
    // Exponential decay is independent of refresh rate. Subtract one UNORM
    // step as well, so quantisation cannot leave permanent faint pixels.
    float decay = exp(-max(iTimeDelta, 0.0) / max(p_persistence, 0.01));
    vec4 history = max(previous * decay - vec4(1.0 / 255.0), vec4(0.0));
    // Fresh text stays exact. Old ink is visible briefly only where it left.
    vec4 c = text.a > 0.0 ? text : history;
    vec3 halo = vec3(0.0);
    float edge = 0.0;
    float radius = max(0.65, iCellSize.y * p_radius * 0.20);
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec4 sampleText = phText(fragCoord + vec2(x, y) * radius);
            if (abs(x) <= 1 && abs(y) <= 1) edge = max(edge, sampleText.a);
            halo += sampleText.rgb * exp(-float(x*x + y*y) * 0.55);
        }
    }
    halo *= p_glow / 5.7;
    // Emission is additive (RGB, zero alpha); compositing keeps the actual
    // glyphs sharp. Feed back only a faint halo to avoid runaway bloom.
    if (!phLight()) {
        c.rgb = max(c.rgb, halo * (1.0 - text.a));
        // A dark backing behind glyph edges preserves text over white-hot
        // reactor highlights. Keep original premultiplied glyph RGB intact.
        c.a = max(c.a, text.a + p_readability * edge * (1.0 - text.a));
    }
    fragColor = clamp(c, 0.0, 1.0);
}
