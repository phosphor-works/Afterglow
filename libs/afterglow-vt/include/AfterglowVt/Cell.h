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
class Cell
{
public:
    constexpr Cell() = default;

    constexpr char32_t codepoint() const { return m_cp; }
    constexpr std::uint32_t styleId() const { return m_packed & StyleMask; }
    constexpr int width() const { return int((m_packed >> WidthShift) & 3); }
    constexpr bool hasGrapheme() const { return (m_packed & GraphemeBit) != 0; }
    constexpr bool isSpacerHead() const { return (m_packed & SpacerHeadBit) != 0; }
    constexpr bool isTail() const { return width() == 0; }
    /// True for a cell nothing was ever printed to (renders as blank).
    constexpr bool isEmpty() const { return m_cp == 0; }

    constexpr void setCodepoint(char32_t cp) { m_cp = cp; }
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

    char32_t m_cp = 0;
    std::uint32_t m_packed = 1u << WidthShift;
};

static_assert(sizeof(Cell) == 8);

/// Extra codepoints of a grapheme cluster, after the base codepoint stored in
/// the cell. Keyed by column; allocated only for rows that need it.
using GraphemeMap = std::unordered_map<std::uint16_t, std::u32string>;

struct Row
{
    std::vector<Cell> cells;
    std::unique_ptr<GraphemeMap> graphemes;
    /// The logical line continues on the next row (soft wrap).
    bool wrapped = false;
    /// A shell prompt starts on this row (OSC 133;A).
    bool promptStart = false;

    Row() = default;
    explicit Row(int cols)
        : cells(std::size_t(cols))
    {
    }
};

} // namespace AfterglowVt
