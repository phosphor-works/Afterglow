// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// System audio for TERMINAL packs. Including this module is the opt-in:
// Afterglow captures playback through CAVA only while a visible window has a
// pack that includes it. Without it, the levels and the spectrum read zero.
//
// The spectrum is 64 mono logarithmic bands from 50 Hz to 10 kHz. The levels
// are smoothed RMS energy with automatic gain: bass is bands 0-19, mids 20-47
// and treble 48-63. They are visual levels, not calibrated measurements.

#ifndef AFTERGLOW_TERMINAL_AUDIO_GLSL
#define AFTERGLOW_TERMINAL_AUDIO_GLSL

#include <terminal_uniforms.glsl>

// A 64x1 R32F texture. Read it through the helpers or texelFetch.
layout(binding = 10) uniform sampler2D uAudioSpectrum;

// One band; zero outside 0..iAudioSpectrumSize-1.
float audioBar(int index) {
    if (index < 0 || index >= iAudioSpectrumSize) return 0.0;
    return texelFetch(uAudioSpectrum, ivec2(index, 0), 0).r;
}

// The spectrum interpolated at a position from 0 (lowest) to 1 (highest).
float audioBarSmooth(float position) {
    float index = clamp(position, 0.0, 1.0) * float(max(iAudioSpectrumSize - 1, 0));
    int lo = int(floor(index));
    return mix(audioBar(lo), audioBar(min(lo + 1, iAudioSpectrumSize - 1)), fract(index));
}

float getBass() { return uAudioLevels.x; }
float getMids() { return uAudioLevels.y; }
float getTreble() { return uAudioLevels.z; }
float getOverall() { return uAudioLevels.w; }

#endif // AFTERGLOW_TERMINAL_AUDIO_GLSL
