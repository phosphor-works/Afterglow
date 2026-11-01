// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace Afterglow {

struct AudioSettings
{
    bool enabled = true; ///< Capture only when a visible shader opts in.
    QString inputMethod = QStringLiteral("auto");
    QString inputSource = QStringLiteral("auto");
    // The spectrum analysis behind the audio uniforms.
    int framerate = 60; ///< spectrum updates per second
    int smoothing = 65; ///< cava noise reduction, 0 (fast) to 100 (smooth)
    double extraSmoothing = 0.2; ///< share of the previous frame kept on each update
    int lowerCutoff = 50; ///< Hz
    int higherCutoff = 10000; ///< Hz
    bool operator==(const AudioSettings&) const = default;
};

} // namespace Afterglow
