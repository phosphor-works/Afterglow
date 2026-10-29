// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/afterglowrender_export.h>

#include <AfterglowVt/Search.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/Terminal.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace AfterglowRender {

/// RGBA8 in memory order (r, g, b, a), matching a UNormByte4 vertex attribute.
inline constexpr std::uint32_t packRgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
{
    return std::uint32_t(r) | (std::uint32_t(g) << 8) | (std::uint32_t(b) << 16) | (std::uint32_t(a) << 24);
}

inline constexpr std::uint32_t packRgba(AfterglowVt::Rgb c, std::uint8_t a = 255)
{
    return packRgba(c.r, c.g, c.b, a);
}

struct SnapshotCell
{
    char32_t cp = 0;
    std::uint32_t fg = 0;
    std::uint32_t bg = 0; ///< alpha 0: default background, nothing to draw
    std::uint32_t underlineColor = 0;
    std::uint32_t grapheme = 0; ///< 1-based index into PaneSnapshot::graphemes, 0 for none
    std::uint32_t link = 0; ///< 1-based index into PaneSnapshot::links, 0 for none
    std::uint16_t attrs = 0;
    AfterglowVt::UnderlineStyle underline = AfterglowVt::UnderlineStyle::None;
    std::uint8_t width = 1; ///< 0 for the tail of a wide character
    // Multicell blocks (OSC 66): scale 0 when the cell is not in one.
    std::uint8_t mcScale = 0;
    std::uint8_t mcWidth = 0; ///< cells per scale step
    std::uint8_t mcFraction = 0; ///< numerator << 4 | denominator
    std::uint8_t mcAlign = 0; ///< vertical << 2 | horizontal
    std::uint8_t mcDx = 0; ///< offset from the block's top-left cell
    std::uint8_t mcDy = 0;
};

/// Search matches to highlight, with the current one drawn differently.
struct SearchHighlight
{
    const std::vector<AfterglowVt::SearchMatch>* matches = nullptr;
    int current = -1;
    std::uint32_t matchColor = 0;
    std::uint32_t currentColor = 0;
};

/// What the renderer needs from one terminal, copied under the terminal lock
/// so shaping and drawing run without holding it. Colours are resolved.
struct AFTERGLOWRENDER_EXPORT PaneSnapshot
{
    int cols = 0;
    int rows = 0;
    /// Rows captured below the viewport, drawn while scrolling by a fraction of a row.
    int extraRows = 0;
    std::vector<SnapshotCell> cells;
    std::vector<std::u32string> graphemes;
    std::vector<std::string> links; ///< OSC 8 URIs of the captured cells

    std::uint32_t defaultFg = 0;
    std::uint32_t defaultBg = 0;

    bool cursorVisible = false;
    int cursorX = 0;
    int cursorY = 0;
    AfterglowVt::CursorShape cursorShape = AfterglowVt::CursorShape::Block;
    bool cursorBlink = false;
    /// Some visible text has SGR 5: the window runs a blink timer.
    bool hasBlinkingText = false;
    std::uint32_t cursorColor = 0;
    /// Text under a block cursor once it has arrived.
    std::uint32_t cursorTextColor = 0;

    std::uint64_t generation = 0;
    /// Bumped by every capture, so renderers can reuse work for unchanged snapshots.
    std::uint64_t version = 0;

    /// The 256-colour palette, for shaders.
    std::array<std::uint32_t, 256> palette{};

    /// Kitty graphics placements in the viewport, in pixels from cell (0, 0).
    std::vector<AfterglowVt::VisiblePlacement> images;

    /// DEC line size per captured row.
    std::vector<AfterglowVt::LineSize> lineSizes;

    const SnapshotCell& at(int x, int y) const { return cells[std::size_t(y) * std::size_t(cols) + std::size_t(x)]; }
    AfterglowVt::LineSize lineSize(int y) const
    {
        return std::size_t(y) < lineSizes.size() ? lineSizes[std::size_t(y)] : AfterglowVt::LineSize::Single;
    }

    /// @param selection drawn with inverted colours when valid for the active screen.
    void capture(const AfterglowVt::Terminal& terminal, const AfterglowVt::Selection* selection = nullptr,
                 const SearchHighlight& search = {}, int extraRows = 0);
};

} // namespace AfterglowRender
