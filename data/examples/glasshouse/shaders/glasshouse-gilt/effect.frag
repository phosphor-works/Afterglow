// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Glasshouse gilt, for the text layer. In the dark world the lamp glows
// softly behind the letters, as light added (it brightens, never covers).
// Every so often light sweeps over the text the way it sweeps the lead;
// iSignal[1] (glaze) sends a stronger sweep with the glass's. The light
// world inks that sweep in gold leaf instead.

#include "glasshouse.glsl"

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    GhWorld w = ghWorld();
    vec2 px = 1.0 / iResolution.xy;
    vec2 uv = fragCoord * px;
    vec4 text = texture(iChannel0, uv);
    vec4 c = text;

    if (!w.ink && p_glow > 0.0) {
        float halo = 0.0;
        float r = max(1.5, iCellSize.y * 0.09);
        for (int j = -2; j <= 2; ++j) {
            for (int i = -2; i <= 2; ++i) {
                halo += texture(iChannel0, uv + vec2(i, j) * r * px).a * exp(-float(i * i + j * j) * 0.35);
            }
        }
        halo = clamp(halo / 9.0, 0.0, 1.0) * (1.0 - text.a);
        c += vec4(w.lamp * halo * p_glow * 0.6, 0.0);
    }

    // The sweep, on the lead's diagonal.
    float aspect = iResolution.x / iResolution.y;
    float diag = uv.x * aspect + uv.y * 0.6;
    float sweep = fract(iTime / p_glintEvery) * 3.0 - 0.5;
    float d = diag - sweep * (aspect + 0.6);
    float glint = exp(-d * d * 180.0) * p_glint;
    float glazeAge = ghAge(1);
    if (glazeAge < 1.8) {
        float g = diag - glazeAge / 1.4 * (aspect + 0.6);
        glint += exp(-g * g * 120.0);
    }
    glint *= text.a;
    c = w.ink ? mix(c, vec4(w.leaf * text.a, text.a), clamp(glint, 0.0, 1.0) * 0.8)
              : c + vec4(mix(w.hot, vec3(1.0), 0.35) * glint * 0.7, 0.0);
    fragColor = c;
}
