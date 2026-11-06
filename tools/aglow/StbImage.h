// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <functional>
#include <string>
#include <string_view>

// aglow's own interface to the vendored stb_image, with aglow's GIF decoder.
// Only StbImage.cpp includes stb_image.h, so that its configuration is the
// same wherever stb_image is used.
namespace Aglow::Stb {

/// The longest side that stb_image decodes (STBI_MAX_DIMENSIONS), the same as
/// AfterglowProto::GraphicsMaxSide. Image.cpp checks that the two agree.
inline constexpr int MaxSide = 10000;

enum class Probe {
    Image, ///< a format that aglow decodes, with a size in the header
    Unknown, ///< not an image that aglow reads
    TooLarge, ///< a side longer than MaxSide
};

/// The size of an encoded image from its header, without decoding it.
Probe probe(std::string_view bytes, int& width, int& height);

/// One decoded frame: RGBA with straight alpha, and the milliseconds that it
/// shows (0 for a still image). Returns false to stop the decoding.
using FrameSink = std::function<bool(const unsigned char* rgba, int width, int height, int delayMs)>;

/// Decodes @p bytes in this process: one frame for a still image, each frame
/// of a GIF in turn until @p sink returns false. False with @p error set when
/// no frame can be decoded. When a GIF is corrupt after its first frame, the
/// frames before the damage are kept and @p warning says so.
bool decode(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning);

} // namespace Aglow::Stb
