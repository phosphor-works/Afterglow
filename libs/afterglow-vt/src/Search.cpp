// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Search.h>
#include <AfterglowVt/TextSearch.h>

namespace AfterglowVt {

std::vector<SearchMatch> findAll(const Screen& screen, std::u32string_view needle, std::size_t limit)
{
    TextSnapshot snapshot;
    for (std::size_t i = 0; i < screen.totalRows(); ++i) {
        if (!appendTextRow(snapshot, screen, i)) {
            break;
        }
    }
    const std::atomic<bool> cancelled{false};
    return searchText(snapshot, needle, false, cancelled, limit).matches;
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
