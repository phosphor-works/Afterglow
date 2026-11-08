// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace Afterglow {

/// Decodes an image that a program sent (a kitty PNG, an iTerm2 inline
/// image) into RGBA8, within the limits of the graphics store: the data, the
/// sides and the decoded size, each checked before the pixels are decoded.
/// @p format names the format ("PNG"), or is empty to take it from the
/// content, among the common formats only (PNG, JPEG, GIF, BMP and WebP).
/// An animation gives its first frame. The panes decode with this, and
/// afterglow_renderpng decodes as they do.
bool decodeBoundedImage(std::string_view data, const char* format, std::uint32_t& width, std::uint32_t& height,
                        std::vector<std::uint8_t>& rgba);

} // namespace Afterglow
