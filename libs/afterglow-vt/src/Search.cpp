// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Search.h>

#include <algorithm>
#include <cwctype>

namespace AfterglowVt {

namespace {

char32_t fold(char32_t c)
{
    return char32_t(std::towlower(wint_t(c)));
}

bool hasUpper(std::u32string_view s)
{
    return std::any_of(s.begin(), s.end(), [](char32_t c) { return std::towlower(wint_t(c)) != wint_t(c); });
}

} // namespace

std::vector<SearchMatch> findAll(const Screen& screen, std::u32string_view needle, std::size_t limit)
{
    std::vector<SearchMatch> out;
    if (needle.empty()) {
        return out;
    }
    const bool ignoreCase = !hasUpper(needle);
    std::u32string pattern(needle);
    if (ignoreCase) {
        std::transform(pattern.begin(), pattern.end(), pattern.begin(), fold);
    }

    // One logical line at a time, with each text position mapped to its cell.
    std::u32string text;
    std::vector<SelectionPoint> cells;
    const std::uint64_t base = screen.rowsDropped();
    const std::size_t total = screen.totalRows();
    std::size_t abs = 0;
    while (abs < total && out.size() < limit) {
        text.clear();
        cells.clear();
        for (;;) {
            const Row& row = screen.absoluteRow(abs);
            for (int x = 0; x < screen.cols(); ++x) {
                const Cell& c = row.at(x);
                if (c.isTail() || c.isSpacerHead()) {
                    continue;
                }
                const SelectionPoint at{base + abs, x};
                text.push_back(c.isEmpty() ? U' ' : (ignoreCase ? fold(c.codepoint()) : c.codepoint()));
                cells.push_back(at);
                for (char32_t e : screen.graphemeExtra(row, x)) {
                    text.push_back(ignoreCase ? fold(e) : e);
                    cells.push_back(at);
                }
            }
            const bool wrapped = row.wrapped && abs + 1 < total;
            ++abs;
            if (!wrapped) {
                break;
            }
        }
        for (std::size_t pos = text.find(pattern); pos != std::u32string::npos && out.size() < limit;
             pos = text.find(pattern, pos + pattern.size())) {
            SearchMatch m{cells[pos], cells[pos + pattern.size() - 1]};
            // Cover a trailing wide character's tail.
            const long long a = screen.absoluteIndexOf(m.end.row);
            if (a >= 0 && m.end.col + 1 < screen.cols()
                && screen.absoluteRow(std::size_t(a)).at(m.end.col + 1).isTail()) {
                ++m.end.col;
            }
            out.push_back(m);
        }
    }
    return out;
}

bool contains(const SearchMatch& m, std::uint64_t row, int col)
{
    if (row < m.start.row || row > m.end.row) {
        return false;
    }
    if (row == m.start.row && col < m.start.col) {
        return false;
    }
    if (row == m.end.row && col > m.end.col) {
        return false;
    }
    return true;
}

} // namespace AfterglowVt
