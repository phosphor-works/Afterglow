// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <functional>
#include <string>
#include <string_view>

// The image decoders of aglow and afterglow-mux: PNG (libpng), JPEG
// (libjpeg-turbo), WebP (libwebp), and GIF and BMP, which this library reads
// itself. No Qt. The three libraries are built in from pinned archives, and
// they still parse what a remote program sends: aglow and afterglow-mux call
// decode() only in a child process with limits on its memory and CPU time.
// Call decode() from one thread: libwebp is built without threads, and
// fills its function tables at the first decode with no lock.
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

/// The size of an encoded image from its header, without decoding it. An
/// Image and a TooLarge image have their size in @p width and @p height.
/// For Unknown both are 0.
Probe probe(std::string_view bytes, int& width, int& height);

/// One decoded frame: RGBA with straight alpha, and the milliseconds that it
/// shows (0 for a still image). The pixels are valid only during the call:
/// the decoder draws the next frame over them. Each frame of an animation
/// is the whole canvas, of the size that probe() gives. Returns false to
/// stop the decoding of an animation. For a still image the result is not
/// read.
using FrameSink = std::function<bool(const unsigned char* rgba, int width, int height, int delayMs)>;

/// Decodes @p bytes in this process: one frame for a still image, each frame
/// of an animation (GIF, WebP) in turn until @p sink returns false. False
/// with @p error set when no frame can be decoded. True with a frame and
/// @p warning set for an animation that is damaged after its first frame:
/// corrupt data of a later frame, or a GIF that is cut short, whose last
/// frame can lack rows. A WebP that is cut short has no frame, because its
/// container must be whole. Throws std::bad_alloc when the memory for a
/// frame is not there. A still lossy WebP with alpha is the exception:
/// libwebp reports a corrupt alpha chunk as a lack of memory, so there the
/// error says that the WebP is corrupt.
bool decode(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning);

} // namespace AfterglowImage
