// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace AfterglowVt {

/// One grid cell, 8 bytes.
///
/// Wide characters occupy a head cell (width 2) followed by a tail cell
/// (width 0). A "spacer head" is the blank left in the last column when a wide
/// character wrapped to the next row; reflow drops it.
///
/// Multicell characters (OSC 66 text sizing) cover a block of cells; each
/// cell of the block is flagged and knows its offset in the block, kept in
/// the codepoint's spare bits. The block's parameters live in the row's
/// multicell map under the block's leftmost column.
class Cell
{
public:
    constexpr Cell() = default;

    constexpr char32_t codepoint() const { return m_cp & CodepointMask; }
    constexpr std::uint32_t styleId() const { return m_packed & StyleMask; }
    constexpr int width() const { return int((m_packed >> WidthShift) & 3); }
    constexpr bool hasGrapheme() const { return (m_packed & GraphemeBit) != 0; }
    constexpr bool isSpacerHead() const { return (m_packed & SpacerHeadBit) != 0; }
    constexpr bool isTail() const { return width() == 0; }
    /// True for a cell nothing was ever printed to (renders as blank).
    constexpr bool isEmpty() const { return codepoint() == 0; }
    constexpr bool isMulticell() const { return (m_cp & MulticellBit) != 0; }
    /// Offset of this cell from its multicell block's top-left cell.
    constexpr int multicellDx() const { return int((m_cp >> DxShift) & 0x3F); }
    constexpr int multicellDy() const { return int((m_cp >> DyShift) & 0x7); }

    /// Sets the character; a plain character is not part of a block.
    constexpr void setCodepoint(char32_t cp) { m_cp = cp & CodepointMask; }
    constexpr void setMulticell(int dx, int dy)
    {
        m_cp = (m_cp & CodepointMask) | MulticellBit | (char32_t(dx & 0x3F) << DxShift) | (char32_t(dy & 0x7) << DyShift);
    }
    constexpr void setStyleId(std::uint32_t id) { m_packed = (m_packed & ~StyleMask) | (id & StyleMask); }
    constexpr void setWidth(int w) { m_packed = (m_packed & ~(3u << WidthShift)) | (std::uint32_t(w & 3) << WidthShift); }
    constexpr void setHasGrapheme(bool on) { m_packed = on ? (m_packed | GraphemeBit) : (m_packed & ~GraphemeBit); }
    constexpr void setSpacerHead(bool on) { m_packed = on ? (m_packed | SpacerHeadBit) : (m_packed & ~SpacerHeadBit); }

    constexpr bool operator==(const Cell&) const = default;

private:
    static constexpr std::uint32_t StyleMask = (1u << 24) - 1;
    static constexpr int WidthShift = 24;
    static constexpr std::uint32_t GraphemeBit = 1u << 26;
    static constexpr std::uint32_t SpacerHeadBit = 1u << 27;
    static constexpr char32_t CodepointMask = (1u << 21) - 1;
    static constexpr int DxShift = 21;
    static constexpr int DyShift = 27;
    static constexpr char32_t MulticellBit = 1u << 31;

    char32_t m_cp = 0;
    std::uint32_t m_packed = 1u << WidthShift;
};

static_assert(sizeof(Cell) == 8);

/// Extra codepoints of a grapheme cluster, after the base codepoint stored in
/// the cell. Keyed by column; allocated only for rows that need it.
using GraphemeMap = std::unordered_map<std::uint16_t, std::u32string>;

/// A multicell block's parameters (OSC 66): it covers scale*width columns
/// by scale rows; text renders at scale * num/den (when num < den) aligned
/// in the block.
struct MulticellParams
{
    std::uint8_t scale = 1; ///< s, 1-7
    std::uint8_t width = 1; ///< cells per scale step, 1-7
    std::uint8_t num = 0; ///< fractional scale, 0-15
    std::uint8_t den = 0;
    std::uint8_t valign = 0; ///< 0 top, 1 bottom, 2 centre
    std::uint8_t halign = 0; ///< 0 left, 1 right, 2 centre
    int columns() const { return scale * width; }
    bool operator==(const MulticellParams&) const = default;
};
/// Keyed by the column where a block starts in this row.
using MulticellMap = std::unordered_map<std::uint16_t, MulticellParams>;

/// DEC line size (DECDWL / DECDHL): the row's characters drawn twice as wide,
/// and for double height, as the top or bottom half.
enum class LineSize : std::uint8_t { Single, DoubleWidth, DoubleTop, DoubleBottom };

struct Row
{
    std::vector<Cell> cells;
    std::unique_ptr<GraphemeMap> graphemes;
    std::unique_ptr<MulticellMap> multicell;
    /// The logical line continues on the next row (soft wrap).
    bool wrapped = false;
    /// A shell prompt starts on this row (OSC 133;A).
    bool promptStart = false;
    LineSize lineSize = LineSize::Single;

    Row() = default;
    explicit Row(int cols)
        : cells(std::size_t(cols))
    {
    }

    /// The cell at @p x. History rows drop their trailing blank cells to
    /// save memory, so cells past the stored ones read as blank.
    const Cell& at(int x) const noexcept
    {
        static const Cell Blank;
        return std::size_t(x) < cells.size() ? cells[std::size_t(x)] : Blank;
    }
};

} // namespace AfterglowVt
