// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalText.h"
#include <algorithm>

namespace Afterglow {
namespace {
void appendCodePoint(QString& text, char32_t c)
{
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
    if (QChar::requiresSurrogates(c)) {
        text += QChar(QChar::highSurrogate(c));
        text += QChar(QChar::lowSurrogate(c));
    } else {
        text += QChar(char16_t(c));
    }
}
// Edit boundaries never split a UTF-16 surrogate pair.
qsizetype commonPrefix(QStringView a, QStringView b)
{
    qsizetype n = 0;
    const auto limit = std::min(a.size(), b.size());
    while (n < limit && a[n] == b[n]) ++n;
    if (n && a[n - 1].isHighSurrogate()) --n;
    return n;
}
qsizetype commonSuffix(QStringView a, QStringView b, qsizetype prefix)
{
    qsizetype n = 0;
    const auto limit = std::min(a.size(), b.size()) - prefix;
    while (n < limit && a[a.size() - 1 - n] == b[b.size() - 1 - n]) ++n;
    if (n && a[a.size() - n].isLowSurrogate()) --n;
    return n;
}
void appendReplacement(QList<TextEdit>& edits, QStringView before, QStringView after, int offset)
{
    const auto prefix = commonPrefix(before, after);
    const auto suffix = commonSuffix(before, after, prefix);
    const auto removed = before.sliced(prefix, before.size() - prefix - suffix);
    const auto inserted = after.sliced(prefix, after.size() - prefix - suffix);
    if (!removed.isEmpty() || !inserted.isEmpty())
        edits.append({offset + int(prefix), removed.toString(), inserted.toString()});
}
}

TerminalText TerminalText::capture(const AfterglowVt::Screen& s, const AfterglowVt::Selection* selection, bool cursorRow, std::u32string_view wordBoundaries)
{
    TerminalText out;
    const auto first = cursorRow ? s.historySize() + std::size_t(s.cursor.y) : s.viewportTop();
    const auto last = cursorRow ? first + 1 : first + std::size_t(s.rows());
    const auto selected = selection && selection->validFor(s);
    const auto range = selected ? selection->range(s, wordBoundaries) : AfterglowVt::Selection::Range{};
    const auto reserve = qsizetype(last - first) * (s.cols() + 1);
    out.text.reserve(reserve);
    out.cells.reserve(reserve);
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
            const auto start = out.text.size();
            appendCodePoint(out.text, cell.isEmpty() ? U' ' : cell.codepoint());
            for (const char32_t c : s.graphemeExtra(row, x)) appendCodePoint(out.text, c);
            if (selected && range.contains(point.row, x)) {
                if (out.selectionStart < 0) out.selectionStart = int(start);
                out.selectionEnd = int(out.text.size());
            }
            for (auto i = start; i < out.text.size(); ++i) out.cells.append(point);
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

QList<TextEdit> textEdits(const QString& before, const QString& after)
{
    QList<TextEdit> edits;
    if (before == after) return edits;
    // Output that scrolls the viewport changes the first line. Report the lines that
    // left the top and the new text, not a replacement of the whole viewport.
    const auto firstLineEnd = before.indexOf(QLatin1Char('\n'));
    if (firstLineEnd >= 0 && commonPrefix(before, after) <= firstLineEnd) {
        for (auto cut = firstLineEnd + 1; cut > 0 && cut < before.size();) {
            const QStringView rest = QStringView(before).sliced(cut);
            const auto lastLine = rest.lastIndexOf(QLatin1Char('\n'));
            if (lastLine >= 0 && commonPrefix(rest, after) > lastLine) {
                edits.append({0, before.left(cut), {}});
                appendReplacement(edits, rest, after, 0);
                return edits;
            }
            const auto next = before.indexOf(QLatin1Char('\n'), cut);
            cut = next < 0 ? -1 : next + 1;
        }
    }
    appendReplacement(edits, before, after, 0);
    return edits;
}
}
