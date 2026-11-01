// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/afterglowvt_export.h>

#include <compare>
#include <cstdint>
#include <string>
#include <limits>
#include <optional>
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

/// A scroll inside the screen that moved cells to other rows: SU, SD, IL, DL,
/// or a line feed or reverse index at a margin of a scroll region. The cells
/// in rows @ref top to @ref bottom and columns @ref left to @ref right moved
/// @ref delta rows (negative is up). Those moved past @ref top or @ref bottom
/// are gone. A scroll into the history moves no cell to another row id, so it
/// is not one of these.
struct RegionScroll
{
    std::uint64_t top = 0; ///< stable row id, inclusive
    std::uint64_t bottom = 0; ///< stable row id, inclusive
    int left = 0; ///< inclusive
    int right = 0; ///< inclusive
    bool fullWidth = true; ///< the columns are the whole width: every cell of the rows moved
    int delta = 0;

    bool contains(SelectionPoint p) const noexcept
    {
        return p.row >= top && p.row <= bottom && (fullWidth || (p.col >= left && p.col <= right));
    }
    /// Where the scroll took the cell at @p p: @p p itself outside the
    /// region, std::nullopt when it left the region.
    std::optional<SelectionPoint> map(SelectionPoint p) const noexcept
    {
        if (!contains(p)) {
            return p;
        }
        const long long row = (long long)p.row + delta;
        if (row < (long long)top || row > (long long)bottom) {
            return std::nullopt;
        }
        return SelectionPoint{std::uint64_t(row), p.col};
    }
};

/// Move one grapheme cell horizontally, or physical rows vertically; clamp to history.
AFTERGLOWVT_EXPORT SelectionPoint moveSelectionCursor(const Screen&, SelectionPoint, int dx, int dy);

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

    /// Starts a new selection at @p at. Use it instead of setting the fields:
    /// it also forgets the last word expansion, which a program that redraws
    /// a row in place would otherwise give to a new double-click there.
    void start(SelectionPoint at, SelectionMode newMode, std::uint64_t epoch, bool activeNow);
    /// False when the selection is inactive or belongs to an older layout.
    bool validFor(const Screen& screen) const noexcept;
    /// Expects validFor(): when the rows are gone the ends come back as they
    /// are, not expanded. A word selection is expanded once and remembered
    /// until its ends, the screen's layout or the boundaries change, a row of
    /// the expansion leaves the history, or start() begins a new selection.
    Range range(const Screen& screen, std::u32string_view wordBoundaries = DefaultWordBoundaries) const;
    /// The selected text, UTF-8. Soft-wrapped rows join without a newline and
    /// trailing blanks of each line are dropped.
    std::string text(const Screen& screen, std::u32string_view wordBoundaries = DefaultWordBoundaries,
                     std::size_t maxBytes = std::numeric_limits<std::size_t>::max(), bool* truncated = nullptr) const;
    /// Moves the selection with the rows @p scroll moved, on the screen it
    /// belongs to. A selection outside the region stays. One wholly inside a
    /// full-width region moves with its text. Returns false when it cannot
    /// follow: a row of it left the region, the region holds only part of it,
    /// or the region is narrower than the screen and holds any of it. The
    /// caller then clears it.
    bool followScroll(const RegionScroll& scroll) noexcept;

private:
    /// The last word expansion. Callers hold the pane lock, as for the rest.
    struct WordCache
    {
        const Screen* screen = nullptr;
        std::uint64_t epoch = 0;
        SelectionPoint anchor;
        SelectionPoint head;
        std::u32string boundaries;
        Range range;
    };
    mutable WordCache m_word;
};

} // namespace AfterglowVt
