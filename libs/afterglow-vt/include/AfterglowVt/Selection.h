// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/afterglowvt_export.h>

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace AfterglowVt {

class Screen;

/// A cell addressed by stable row id (see Screen::rowsDropped()).
struct SelectionPoint
{
    std::uint64_t row = 0;
    int col = 0;
    auto operator<=>(const SelectionPoint&) const = default;
};

enum class SelectionMode : std::uint8_t {
    Normal, ///< character stream
    Word, ///< extends to word boundaries
    Line, ///< extends to whole logical lines
    Block, ///< rectangle
};

/// Characters that end a word for double-click selection, besides whitespace.
inline constexpr std::u32string_view DefaultWordBoundaries = U"`\"'│|:;,()[]{}<>";

/// A selection on one screen. @ref anchor is where it started, @ref head
/// follows the pointer; either may come first.
struct AFTERGLOWVT_EXPORT Selection
{
    SelectionPoint anchor;
    SelectionPoint head;
    SelectionMode mode = SelectionMode::Normal;
    std::uint64_t layoutEpoch = 0;
    bool active = false;

    /// Normalized, mode-expanded range, both ends inclusive.
    struct Range
    {
        SelectionPoint start;
        SelectionPoint end;
        bool block = false;
        int blockLeft = 0;
        int blockRight = 0;

        bool contains(std::uint64_t row, int col) const noexcept;
    };

    /// False when the selection is inactive or belongs to an older layout.
    bool validFor(const Screen& screen) const noexcept;
    Range range(const Screen& screen, std::u32string_view wordBoundaries = DefaultWordBoundaries) const;
    /// The selected text, UTF-8. Soft-wrapped rows join without a newline and
    /// trailing blanks of each line are dropped.
    std::string text(const Screen& screen, std::u32string_view wordBoundaries = DefaultWordBoundaries) const;
};

} // namespace AfterglowVt
