// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Selection.h>
#include <AfterglowVt/afterglowvt_export.h>

#include <string_view>
#include <vector>

namespace AfterglowVt {

class Screen;

struct SearchMatch
{
    SelectionPoint start; ///< first cell, stable row id
    SelectionPoint end; ///< last cell, inclusive
};

/// Finds every occurrence of @p needle in the screen's history and active
/// area, oldest first. Matches span soft-wrapped rows. Smart case: the search
/// ignores case unless @p needle contains an upper-case letter.
AFTERGLOWVT_EXPORT std::vector<SearchMatch> findAll(const Screen& screen, std::u32string_view needle,
                                                    std::size_t limit = 100000);

/// True if the cell lies inside the match.
AFTERGLOWVT_EXPORT bool contains(const SearchMatch& match, std::uint64_t row, int col);

} // namespace AfterglowVt
