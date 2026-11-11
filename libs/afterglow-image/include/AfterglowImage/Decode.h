// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <functional>
#include <string>
#include <string_view>

// The image decoders of aglow and afterglow-mux: PNG (libpng), JPEG
// (libjpeg-turbo), WebP (libwebp), and GIF and BMP, which this library reads
// itself. No Qt. The system's libraries decode, and they still parse what a
// remote program sends: aglow and afterglow-mux call decode() only in a
// child process with limits on its memory and CPU time.
namespace AfterglowImage {

/// The longest side that the decoders take, the same as
/// AfterglowProto::GraphicsMaxSide.
inline constexpr int MaxSide = 10000;

enum class Format {
    Unknown,
    Png,
    Jpeg,
    Gif,
    WebP,
    Bmp,
};

/// The format of @p bytes from its signature.
Format format(std::string_view bytes);

enum class Probe {
    Image, ///< a format that the decoders read, with a size in the header
    Unknown, ///< not an image that they read
    TooLarge, ///< a side longer than MaxSide
};

/// The size of an encoded image from its header, without decoding it. For a
/// TooLarge image, @p width and @p height are its size when the header has
/// one, else 0.
Probe probe(std::string_view bytes, int& width, int& height);

/// One decoded frame: RGBA with straight alpha, and the milliseconds that it
/// shows (0 for a still image). Returns false to stop the decoding.
using FrameSink = std::function<bool(const unsigned char* rgba, int width, int height, int delayMs)>;

/// Decodes @p bytes in this process: one frame for a still image, each frame
/// of an animation (GIF, WebP) in turn until @p sink returns false. False
/// with @p error set when no frame can be decoded. When an animation is
/// corrupt after its first frame, the frames before the damage are kept and
/// @p warning says so.
bool decode(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning);

} // namespace AfterglowImage
