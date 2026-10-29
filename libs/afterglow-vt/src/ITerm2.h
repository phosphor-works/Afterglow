// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Graphics.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace AfterglowVt::detail {

/// An iTerm2 file transfer (OSC 1337 File= or MultipartFile=).
/// https://iterm2.com/documentation-images.html
struct ITerm2File
{
    std::string width; ///< N (cells), Npx, N%, or auto/empty
    std::string height;
    bool preserveAspectRatio = true;
    bool inlineImage = false; ///< inline=1: show it; otherwise it is a download, which is not offered
    bool doNotMoveCursor = false;
    std::size_t size = 0; ///< as declared, 0 if not
};

/// Reads the key=value;... arguments of a File= or MultipartFile= command.
void parseITerm2Args(std::string_view args, ITerm2File& out);

/// The size in pixels to show an image of @p naturalW by @p naturalH at,
/// per the arguments, within a terminal of @p cols by @p rows cells.
/// False if the result is unusable.
bool iterm2ImageSize(const ITerm2File& file, std::uint32_t naturalW, std::uint32_t naturalH, int cellW, int cellH,
                     int cols, int rows, std::uint32_t& width, std::uint32_t& height);

/// Resamples straight-alpha RGBA: area averaging when shrinking, bilinear
/// when enlarging.
ImageFrame scaleFrame(const ImageFrame& in, std::uint32_t width, std::uint32_t height);

} // namespace AfterglowVt::detail
