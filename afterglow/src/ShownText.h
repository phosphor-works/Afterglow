// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <algorithm>

namespace Afterglow {

/// At most @p limit UTF-16 units of @p text, without splitting a surrogate pair.
inline QString textPrefix(const QString& text, qsizetype limit)
{
    if (text.size() <= limit) return text;
    return text.left(limit > 0 && text.at(limit).isLowSurrogate() ? limit - 1 : limit);
}

/// Bidirectional overrides and invisible formatting, which can disguise
/// text, and the line and paragraph separators, which Qt breaks lines at.
inline bool disguisesText(char16_t u)
{
    return (u >= 0x200B && u <= 0x200F) || (u >= 0x202A && u <= 0x202E) || (u >= 0x2060 && u <= 0x2069) || u == 0xFEFF
        || u == 0x061C || u == 0x00AD || u == 0x2028 || u == 0x2029;
}

/// Program-supplied text shown outside the grid: without what disguisesText
/// names. At most @p limit UTF-16 units, without splitting a surrogate pair.
inline QString sanitizedText(const QString& s, qsizetype limit)
{
    QString out;
    out.reserve(std::min(s.size(), limit));
    for (const QChar c : s) {
        if (!disguisesText(c.unicode())) out.append(c);
    }
    return textPrefix(out, limit);
}

} // namespace Afterglow
