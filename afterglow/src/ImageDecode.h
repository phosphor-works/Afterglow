// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowVt/Graphics.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace Afterglow {

/// Decodes an image that a program sent (a kitty PNG, an iTerm2 inline
/// image) into RGBA8, within the limits of the graphics store: the data, the
/// sides and the decoded size, each checked before the pixels are decoded.
/// @p format names the format ("PNG"), or is empty to take it from the
/// content, among the common formats only (PNG, JPEG, GIF, BMP and WebP).
/// An animation gives its first frame. afterglow_renderpng decodes with
/// this, in its own process: the panes decode with ProgramImages, in
/// processes with limits.
bool decodeBoundedImage(std::string_view data, const char* format, std::uint32_t& width, std::uint32_t& height,
                        std::vector<std::uint8_t>& rgba);

/// decodeBoundedImage() of a common format, with each frame of an
/// animation (GIF, WebP) and the milliseconds that it shows: whole canvases,
/// at most AfterglowProto::GraphicsMaxFrames of them and @p maxBytes of
/// RGBA8 together. An animation stops at the last frame that fits, or before
/// a frame that does not decode. A still image has one frame of 0 ms.
bool decodeBoundedFrames(std::string_view data, std::size_t maxBytes, std::vector<AfterglowVt::ImageFrame>& frames,
                         std::vector<std::uint32_t>& delays);

} // namespace Afterglow
