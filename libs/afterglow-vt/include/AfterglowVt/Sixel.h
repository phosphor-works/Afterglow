// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Graphics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace AfterglowVt {

/// The largest image decodeSixel accepts: each side, and the pixel count.
/// XTSMGRAPHICS reports no more.
inline constexpr int SixelMaxSide = 10000;
inline constexpr std::size_t SixelMaxPixels = std::size_t(4096) * 4096;

/// Sixel colour registers, 0xRRGGBB.
using SixelPalette = std::array<std::uint32_t, 1024>;

/// The VT340's sixteen default colours, the rest black.
AFTERGLOWVT_EXPORT SixelPalette defaultSixelPalette();

/// Decodes the data of a sixel DCS (after "q") into RGBA. @p p2 is the
/// background-select parameter: 1 leaves unpainted pixels transparent,
/// otherwise they take @p background (0xRRGGBB). Colour definitions update
/// @p palette. Returns false for an empty or oversized image.
AFTERGLOWVT_EXPORT bool decodeSixel(std::string_view data, int p2, std::uint32_t background, SixelPalette& palette,
                                    ImageFrame& out);

} // namespace AfterglowVt
