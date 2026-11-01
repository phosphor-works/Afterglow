// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Selection.h>

#include "Utf8.h"

#include <algorithm>

namespace AfterglowVt {

namespace {

/// Walks cells across soft-wrapped rows.
struct CellWalker
{
    const Screen* scr;
    long long abs;
    int col;

    const Screen& screen() const { return *scr; }
    const Row& row() const { return scr->absoluteRow(std::size_t(abs)); }
    char32_t cp() const
    {
        const Cell& c = row().at(col);
        return c.isTail() ? 0xFFFFFFFF : c.codepoint();
    }
    // A spacer head (the blank a wrapped wide character left in the last
    // column) is not a cell of the line, as text() and reflow agree: the
    // walk steps over it.
    bool spacer() const { return row().at(col).isSpacerHead(); }
    bool prev()
    {
        do {
            if (!step(-1)) {
                return false;
            }
        } while (spacer());
        return true;
    }
    bool next()
    {
        do {
            if (!step(1)) {
                return false;
            }
        } while (spacer());
        return true;
    }

private:
    bool step(int dir)
    {
        if (dir < 0) {
            if (col > 0) {
                --col;
                return true;
            }
            if (abs > 0 && scr->absoluteRow(std::size_t(abs - 1)).wrapped) {
                --abs;
                col = scr->cols() - 1;
                return true;
            }
            return false;
        }
        if (col + 1 < scr->cols()) {
            ++col;
            return true;
        }
        if (row().wrapped && std::size_t(abs + 1) < scr->totalRows()) {
            ++abs;
            col = 0;
            return true;
        }
        return false;
    }
};

bool isWordChar(char32_t cp, std::u32string_view boundaries)
{
    if (cp == 0xFFFFFFFF) {
        return true; // wide-character tail belongs to its head
    }
    if (cp == 0 || cp == U' ' || cp == U'\t' || cp == 0xA0) {
        return false;
    }
    return boundaries.find(cp) == std::u32string_view::npos;
}

} // namespace

bool Selection::Range::contains(std::uint64_t row, int col) const noexcept
{
    if (row < start.row || row > end.row) {
        return false;
    }
    if (block) {
        return col >= blockLeft && col <= blockRight;
    }
    if (row == start.row && col < start.col) {
        return false;
    }
    if (row == end.row && col > end.col) {
        return false;
    }
    return true;
}

bool Selection::validFor(const Screen& screen) const noexcept
{
    return active && layoutEpoch == screen.layoutEpoch() && screen.absoluteIndexOf(anchor.row) >= 0
        && screen.absoluteIndexOf(head.row) >= 0;
}

bool Selection::followScroll(const RegionScroll& scroll) noexcept
{
    if (!active) {
        return true;
    }
    const std::uint64_t first = std::min(anchor.row, head.row);
    const std::uint64_t last = std::max(anchor.row, head.row);
    if (last < scroll.top || first > scroll.bottom) {
        return true;
    }
    // Only a full-width region moves whole rows; anything less splits the
    // selected text between cells that moved and cells that stayed.
    if (!scroll.fullWidth || first < scroll.top || last > scroll.bottom) {
        return false;
    }
    const auto movedAnchor = scroll.map(anchor);
    const auto movedHead = scroll.map(head);
    if (!movedAnchor || !movedHead) {
        return false;
    }
    anchor = *movedAnchor;
    head = *movedHead;
    return true;
}

Selection::Range Selection::range(const Screen& screen, std::u32string_view boundaries) const
{
    Range r;
    r.start = std::min(anchor, head);
    r.end = std::max(anchor, head);
    if (screen.absoluteIndexOf(r.start.row) < 0 || screen.absoluteIndexOf(r.end.row) < 0) {
        return r; // the rows are gone: nothing to expand (callers check validFor first)
    }
    const std::uint64_t base = screen.rowsDropped();
    const int lastCol = screen.cols() - 1;
    r.start.col = std::clamp(r.start.col, 0, lastCol);
    r.end.col = std::clamp(r.end.col, 0, lastCol);

    switch (mode) {
    case SelectionMode::Normal:
        break;
    case SelectionMode::Block:
        r.block = true;
        r.blockLeft = std::clamp(std::min(anchor.col, head.col), 0, lastCol);
        r.blockRight = std::clamp(std::max(anchor.col, head.col), 0, lastCol);
        break;
    case SelectionMode::Word: {
        // The walk crosses every soft-wrapped row of the word, and the
        // renderer asks for the range each frame: expand once per selection.
        if (m_word.screen == &screen && m_word.epoch == screen.layoutEpoch() && m_word.anchor == anchor
            && m_word.head == head && m_word.boundaries == boundaries) {
            return m_word.range;
        }
        CellWalker s{&screen, screen.absoluteIndexOf(r.start.row), r.start.col};
        const bool startWord = isWordChar(s.cp(), boundaries);
        for (CellWalker probe = s; probe.prev() && isWordChar(probe.cp(), boundaries) == startWord && startWord;) {
            s = probe;
        }
        CellWalker e{&screen, screen.absoluteIndexOf(r.end.row), r.end.col};
        const bool endWord = isWordChar(e.cp(), boundaries);
        for (CellWalker probe = e; probe.next() && isWordChar(probe.cp(), boundaries) == endWord && endWord;) {
            e = probe;
        }
        r.start = {base + std::uint64_t(s.abs), s.col};
        r.end = {base + std::uint64_t(e.abs), e.col};
        break;
    }
    case SelectionMode::Line: {
        long long s = screen.absoluteIndexOf(r.start.row);
        while (s > 0 && screen.absoluteRow(std::size_t(s - 1)).wrapped) {
            --s;
        }
        long long e = screen.absoluteIndexOf(r.end.row);
        while (std::size_t(e + 1) < screen.totalRows() && screen.absoluteRow(std::size_t(e)).wrapped) {
            ++e;
        }
        r.start = {base + std::uint64_t(s), 0};
        r.end = {base + std::uint64_t(e), lastCol};
        break;
    }
    }
    // A wide character is selected whole.
    if (const long long a = screen.absoluteIndexOf(r.start.row); a >= 0 && r.start.col > 0) {
        if (screen.absoluteRow(std::size_t(a)).at(r.start.col).isTail()) --r.start.col;
    }
    if (const long long a = screen.absoluteIndexOf(r.end.row); a >= 0 && r.end.col + 1 <= lastCol) {
        if (screen.absoluteRow(std::size_t(a)).at(r.end.col + 1).isTail()) {
            ++r.end.col;
        }
    }
    if (mode == SelectionMode::Word) {
        m_word = {&screen, screen.layoutEpoch(), anchor, head, std::u32string(boundaries), r};
    }
    return r;
}

std::string Selection::text(const Screen& screen, std::u32string_view boundaries, std::size_t maxBytes, bool* truncated) const
{
    if (truncated) *truncated = false;
    if (!validFor(screen)) {
        return {};
    }
    const Range r = range(screen, boundaries);
    const long long first = screen.absoluteIndexOf(r.start.row);
    const long long last = screen.absoluteIndexOf(r.end.row);
    std::string out;
    for (long long a = first; a <= last; ++a) {
        const Row& row = screen.absoluteRow(std::size_t(a));
        const std::uint64_t id = screen.rowsDropped() + std::uint64_t(a);
        int c0 = 0;
        int c1 = screen.cols() - 1;
        if (r.block) {
            c0 = r.blockLeft;
            c1 = r.blockRight;
        } else {
            if (id == r.start.row) {
                c0 = r.start.col;
            }
            if (id == r.end.row) {
                c1 = r.end.col;
            }
        }
        if (c0 > 0 && row.at(c0).isTail()) --c0;
        std::string line;
        for (int x = c0; x <= c1; ++x) {
            const Cell& c = row.at(x);
            // A multicell block's text belongs to its top-left cell only.
            if (c.isTail() || c.isSpacerHead()
                || (c.isMulticell() && (c.multicellDx() != 0 || c.multicellDy() != 0))) {
                continue;
            }
            detail::appendUtf8(line, c.isEmpty() ? U' ' : c.codepoint());
            for (char32_t e : screen.graphemeExtra(row, x)) {
                detail::appendUtf8(line, e);
            }
        }
        const bool joinsNext = !r.block && row.wrapped && a < last;
        if (!joinsNext) {
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
        }
        if (line.size() > maxBytes - out.size()) {
            auto bytes = maxBytes - out.size();
            while (bytes && bytes < line.size() && (static_cast<unsigned char>(line[bytes]) & 0xc0) == 0x80) --bytes;
            out.append(line, 0, bytes);
            if (truncated) *truncated = true;
            return out;
        }
        out += line;
        if (a < last && !joinsNext) {
            if (out.size() == maxBytes) { if (truncated) *truncated = true; return out; }
            out += '\n';
        }
    }
    return out;
}

} // namespace AfterglowVt

namespace AfterglowVt {
SelectionPoint moveSelectionCursor(const Screen& screen, SelectionPoint point, int dx, int dy)
{
    const auto first = screen.rowsDropped();
    const auto last = first + screen.totalRows() - 1;
    point.row = std::clamp(point.row, first, last);
    point.row = std::uint64_t(std::clamp(std::int64_t(point.row) + dy, std::int64_t(first), std::int64_t(last)));
    point.col = std::clamp(point.col, 0, screen.cols() - 1);
    const auto tail = [&](SelectionPoint p) { return screen.absoluteRow(std::size_t(p.row - first)).at(p.col).isTail(); };
    if (dx) {
        const int step = dx > 0 ? 1 : -1;
        do {
            const auto before = point;
            point.col += step;
            if (point.col >= screen.cols()) {
                if (point.row < last) { ++point.row; point.col = 0; }
                else point.col = screen.cols() - 1;
            } else if (point.col < 0) {
                if (point.row > first) { --point.row; point.col = screen.cols() - 1; }
                else point.col = 0;
            }
            if (point == before) break;
        } while (tail(point));
    }
    while (point.col > 0 && tail(point)) --point.col;
    return point;
}
}
