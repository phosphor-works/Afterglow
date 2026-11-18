// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <functional>
#include <string>
#include <string_view>

// The image decoders of aglow and aglow-mux: PNG and APNG (libpng), JPEG
// (libjpeg-turbo), WebP (libwebp), and GIF, BMP, TIFF and netPBM, which this
// library reads itself, with Little CMS for the colour conversion to sRGB.
// No Qt. The four libraries are built in from pinned archives, and they
// still parse what a remote program sends: aglow and aglow-mux call
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
    Tiff,
    Pnm, ///< netPBM: PBM, PGM, PPM and PAM
};

/// The format of @p bytes from its signature. Inline: a client of a decoding
/// process (mux/src/ImageDecoder.cpp) checks it without the decoders.
inline Format format(std::string_view bytes)
{
    if (bytes.starts_with("\x89PNG\r\n\x1a\n")) return Format::Png;
    if (bytes.starts_with("\xff\xd8\xff")) return Format::Jpeg;
    if (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a")) return Format::Gif;
    if (bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP") return Format::WebP;
    if (bytes.starts_with("BM")) return Format::Bmp;
    if (bytes.starts_with(std::string_view("II*\0", 4)) || bytes.starts_with(std::string_view("MM\0*", 4))) return Format::Tiff;
    // P1 to P7, then white space.
    if (bytes.size() >= 3 && bytes[0] == 'P' && bytes[1] >= '1' && bytes[1] <= '7'
        && (bytes[2] == ' ' || bytes[2] == '\t' || bytes[2] == '\n' || bytes[2] == '\r')) {
        return Format::Pnm;
    }
    return Format::Unknown;
}

enum class Probe {
    Image, ///< a format that the decoders read, with a size in the header
    Unknown, ///< not an image that they read
    TooLarge, ///< a side longer than MaxSide
};

/// The size of an encoded image from its header, without decoding it. An
/// Image and a TooLarge image have their size in @p width and @p height.
/// For Unknown both are 0.
Probe probe(std::string_view bytes, int& width, int& height);

/// One decoded frame: RGBA with straight alpha in sRGB, and the milliseconds
/// that it shows (0 for a still image). A file with an ICC profile or a PNG
/// cICP chunk converts to sRGB before the sink gets it. The pixels are valid
/// only during the call: the decoder draws the next frame over them. Each
/// frame, still or of an animation, is the whole canvas, of the size that
/// probe() gives. Returns false to stop the decoding of an animation: the
/// sink is not called again. For a still image the result is not read.
using FrameSink = std::function<bool(const unsigned char* rgba, int width, int height, int delayMs)>;

/// What decode() gives of a PNG.
enum class PngFrames {
    Animation, ///< each frame of an APNG
    /// Its default image (IDAT) as a still image, as a decoder that reads
    /// no APNG shows it: kitty's graphics protocol (f=100) shows that image,
    /// also when the animation does not show it.
    DefaultImage,
};

/// Decodes @p bytes in this process: one frame for a still image, each frame
/// of an animation (GIF, WebP, APNG) in turn until @p sink returns false. False
/// with @p error set when no frame can be decoded. True with a frame and
/// @p warning set for an animation that stops early: corrupt data of a
/// later frame, an APNG frame outside the canvas, with no image data or
/// with its chunks out of order, a GIF that is cut short, whose last frame
/// can lack rows, or a WebP animation whose times add up past INT_MAX
/// milliseconds (it stops before the frame that passes it). An APNG whose
/// first frame is not valid in one of those ways gives its default image,
/// as a still image with a warning. A WebP that is cut short has no frame,
/// because its container must be whole. Other still images set no warning:
/// a JPEG whose data ends early or holds a bad marker gives its picture,
/// with grey where the data is missing, as browsers show it. Throws
/// std::bad_alloc when the memory for a frame is not there. A still lossy
/// WebP with alpha is the exception: libwebp reports a corrupt alpha chunk
/// as a lack of memory, so there the error says that the WebP is corrupt.
bool decode(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning, PngFrames pngFrames = PngFrames::Animation);

} // namespace AfterglowImage
