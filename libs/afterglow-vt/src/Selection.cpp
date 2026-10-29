// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Selection.h>

#include <algorithm>

namespace AfterglowVt {

namespace {

void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

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
        const Cell& c = row().cells[std::size_t(col)];
        return c.isTail() ? 0xFFFFFFFF : c.codepoint();
    }
    bool prev()
    {
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
    bool next()
    {
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

Selection::Range Selection::range(const Screen& screen, std::u32string_view boundaries) const
{
    Range r;
    r.start = std::min(anchor, head);
    r.end = std::max(anchor, head);
    const std::uint64_t base = screen.rowsDropped();
    const int lastCol = screen.cols() - 1;

    switch (mode) {
    case SelectionMode::Normal:
        break;
    case SelectionMode::Block:
        r.block = true;
        r.blockLeft = std::min(anchor.col, head.col);
        r.blockRight = std::max(anchor.col, head.col);
        break;
    case SelectionMode::Word: {
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
    if (const long long a = screen.absoluteIndexOf(r.end.row); a >= 0 && r.end.col + 1 <= lastCol) {
        if (screen.absoluteRow(std::size_t(a)).cells[std::size_t(r.end.col + 1)].isTail()) {
            ++r.end.col;
        }
    }
    return r;
}

std::string Selection::text(const Screen& screen, std::u32string_view boundaries) const
{
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
        std::string line;
        for (int x = c0; x <= c1; ++x) {
            const Cell& c = row.cells[std::size_t(x)];
            if (c.isTail() || c.isSpacerHead()) {
                continue;
            }
            appendUtf8(line, c.isEmpty() ? U' ' : c.codepoint());
            for (char32_t e : screen.graphemeExtra(row, x)) {
                appendUtf8(line, e);
            }
        }
        const bool joinsNext = !r.block && row.wrapped && a < last;
        if (!joinsNext) {
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
        }
        out += line;
        if (a < last && !joinsNext) {
            out += '\n';
        }
    }
    return out;
}

} // namespace AfterglowVt
