// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The Phosphor spectrum, for the Phosphor packs and for packs that want the
// same look: the four brand colours (cyan, blue, purple, rose) as one
// gradient. The rim that carries it is in rim.glsl, which this file includes.
//
// This file is part of each pack that includes it. Read no clock and no event
// age here: the pack would then render continuously.
//
// A pack that includes this file declares these parameters:
//   followTheme  bool   take the colours from the terminal palette
//   stop0-stop3  color  the gradient, from its start to its end

#ifndef PHOSPHOR_SPECTRUM_GLSL
#define PHOSPHOR_SPECTRUM_GLSL

#include <rim.glsl>

// The gradient's colours, sRGB. With followTheme they are the theme's bright
// cyan, blue, magenta and red, so a palette from a wallpaper carries through.
vec3 spectrumStop(int i) {
    if (p_followTheme > 0.5) {
        const int index[4] = int[4](14, 12, 13, 9);
        return paletteColor(index[clamp(i, 0, 3)]);
    }
    return i <= 0 ? p_stop0.rgb : i == 1 ? p_stop1.rgb : i == 2 ? p_stop2.rgb : p_stop3.rgb;
}

// The gradient at t, 0 to 1, sRGB. The stops are at 0, 0.34, 0.68 and 1, as
// in the Phosphor design language.
vec3 spectrum(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 c = mix(spectrumStop(0), spectrumStop(1), clamp(t / 0.34, 0.0, 1.0));
    c = mix(c, spectrumStop(2), clamp((t - 0.34) / 0.34, 0.0, 1.0));
    return mix(c, spectrumStop(3), clamp((t - 0.68) / 0.32, 0.0, 1.0));
}

// The colour that marks where a pane's shell runs (paneContext()): cyan for
// a remote host, rose for an elevated user, purple for a container.
vec3 spectrumContext(int context) {
    return context == 2 ? spectrumStop(3) : context == 3 ? spectrumStop(2) : spectrumStop(0);
}

#endif // PHOSPHOR_SPECTRUM_GLSL
