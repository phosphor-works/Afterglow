// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Aglow {

/// A decoded image: one RGBA frame, or the full canvas of each frame of an
/// animation, with straight (not premultiplied) alpha.
struct Image
{
    struct Frame
    {
        std::string rgba;
        int delayMs = 0;
    };
    int width = 0;
    int height = 0;
    std::vector<Frame> frames;
};

/// The size of an encoded image without decoding it. False when the format is unknown.
bool imageSize(std::string_view bytes, int& width, int& height);
/// Whether @p bytes are a PNG file.
bool isPng(std::string_view bytes);
/// Decodes PNG, JPEG, GIF (with every frame), BMP, PSD, TGA, HDR, PIC and
/// PNM. nullopt with @p error set for data it cannot read.
std::optional<Image> decodeImage(std::string_view bytes, std::string& error);
/// Scales every frame to @p width x @p height.
void resizeImage(Image& image, int width, int height);
void mirrorImage(Image& image, bool horizontal, bool vertical);
/// Draws every frame over an opaque colour, so that it has no transparency left.
void flattenImage(Image& image, std::uint8_t red, std::uint8_t green, std::uint8_t blue);
/// Whether every pixel of every frame is opaque.
bool imageOpaque(const Image& image);

} // namespace Aglow
