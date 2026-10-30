// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalText.h"
#include <algorithm>

namespace Afterglow {
TerminalText TerminalText::capture(const AfterglowVt::Screen& s, const AfterglowVt::Selection* selection, bool cursorRow, std::u32string_view wordBoundaries)
{
    TerminalText out;
    const auto first = cursorRow ? s.historySize() + std::size_t(s.cursor.y) : s.viewportTop();
    const auto last = cursorRow ? first + 1 : first + std::size_t(s.rows());
    const auto selected = selection && selection->validFor(s);
    const auto range = selected ? selection->range(s, wordBoundaries) : AfterglowVt::Selection::Range{};
    for (auto y = first; y < last; ++y) {
        const auto& row = s.absoluteRow(y);
        int end = s.cols();
        if (!row.wrapped) {
            while (end > 0 && row.at(end - 1).isEmpty()) --end;
            if (y == s.historySize() + std::size_t(s.cursor.y)) end = std::max(end, s.cursor.x);
        }
        for (int x = 0; x < end; ++x) {
            const auto& cell = row.at(x);
            if (cell.isTail() || cell.isSpacerHead() || (cell.isMulticell() && (cell.multicellDx() || cell.multicellDy()))) continue;
            const AfterglowVt::SelectionPoint point{s.rowsDropped() + y, x};
            const char32_t cp = cell.isEmpty() ? U' ' : cell.codepoint();
            QString grapheme = QString::fromUcs4(&cp, 1);
            const auto extra = s.graphemeExtra(row, x);
            grapheme += QString::fromUcs4(extra.data(), qsizetype(extra.size()));
            if (selected && range.contains(point.row, x)) {
                if (out.selectionStart < 0) out.selectionStart = int(out.text.size());
                out.selectionEnd = int(out.text.size() + grapheme.size());
            }
            out.text += grapheme;
            for (qsizetype i = 0; i < grapheme.size(); ++i) out.cells.append(point);
        }
        if (y == s.historySize() + std::size_t(s.cursor.y)) {
            out.cursor = int(out.text.size());
            for (int i = 0; i < out.cells.size(); ++i) {
                if (out.cells[i].row == s.rowsDropped() + y && out.cells[i].col >= s.cursor.x) { out.cursor = i; break; }
            }
        }
        if (y + 1 < last && !row.wrapped) {
            out.text += QLatin1Char('\n');
            out.cells.append({s.rowsDropped() + y, end});
        }
    }
    return out;
}
int TerminalText::offset(AfterglowVt::SelectionPoint point) const
{
    const auto it = std::lower_bound(cells.cbegin(), cells.cend(), point);
    return int(it - cells.cbegin());
}
}
