// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <algorithm>

namespace Afterglow {

/// At most @p limit UTF-16 units of @p text, without splitting a surrogate pair.
inline QString textPrefix(const QString& text, qsizetype limit)
{
    if (text.size() <= limit)
        return text;
    return text.left(limit > 0 && text.at(limit).isLowSurrogate() ? limit - 1 : limit);
}

/// Bidirectional overrides and invisible formatting, which can disguise
/// text, and the line and paragraph separators, which Qt breaks lines at.
inline bool disguisesText(char16_t u)
{
    return (u >= 0x200B && u <= 0x200F) || (u >= 0x202A && u <= 0x202E) || (u >= 0x2060 && u <= 0x2069) || u == 0xFEFF
        || u == 0x061C || u == 0x00AD || u == 0x2028 || u == 0x2029;
}

/// disguisesText for a code point, with the invisible ones past the BMP: the
/// tag characters and the supplementary variation selectors.
inline bool disguisesCodePoint(char32_t c)
{
    return (c <= 0xFFFF && disguisesText(char16_t(c))) || (c >= 0xE0000 && c <= 0xE007F)
        || (c >= 0xE0100 && c <= 0xE01EF);
}

/// The code points of @p s that @p keep takes, at most @p limit UTF-16 units
/// of them, without splitting a surrogate pair. A lone surrogate is a code
/// point of its own.
template<typename Keep>
QString keptText(const QString& s, qsizetype limit, Keep keep)
{
    QString out;
    out.reserve(std::min(s.size(), limit));
    // Past the limit, textPrefix needs only the next unit: a long title stops
    // here, not at its end.
    for (qsizetype i = 0; i < s.size() && out.size() <= limit; ++i) {
        const QChar c = s.at(i);
        if (c.isHighSurrogate() && i + 1 < s.size() && s.at(i + 1).isLowSurrogate()) {
            const QChar low = s.at(++i);
            if (keep(QChar::surrogateToUcs4(c, low))) {
                out.append(c);
                out.append(low);
            }
        } else if (keep(char32_t(c.unicode()))) {
            out.append(c);
        }
    }
    return textPrefix(out, limit);
}

/// Program-supplied text shown outside the grid: without what
/// disguisesCodePoint names. At most @p limit UTF-16 units, without splitting
/// a surrogate pair.
inline QString sanitizedText(const QString& s, qsizetype limit)
{
    return keptText(s, limit, [](char32_t c) {
        return !disguisesCodePoint(c);
    });
}

/// sanitizedText() without control characters too: one line of text that is
/// kept, such as a title that a client sets.
inline QString sanitizedLine(const QString& s, qsizetype limit)
{
    return keptText(s, limit, [](char32_t c) {
        return QChar::category(c) != QChar::Other_Control && !disguisesCodePoint(c);
    });
}

} // namespace Afterglow
