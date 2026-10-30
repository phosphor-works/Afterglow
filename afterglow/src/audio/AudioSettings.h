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
    bool operator==(const AudioSettings&) const = default;
};

} // namespace Afterglow
