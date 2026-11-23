// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "Color.h"

#include <AfterglowImage/Decode.h>

#include <cstdint>
#include <string>
#include <string_view>

// The decoder of each format. decode() in Decode.cpp picks one by the
// signature, so each of them gets bytes that start with its own.
namespace AfterglowImage::detail {

inline std::uint8_t at(std::string_view bytes, std::size_t index)
{
    return std::uint8_t(bytes[index]);
}

/// The @p count bytes (4 at most) at @p index as a little-endian number.
inline std::uint32_t little(std::string_view bytes, std::size_t index, int count)
{
    std::uint32_t value = 0;
    for (int i = count; i-- > 0;)
        value = value << 8 | at(bytes, index + std::size_t(i));
    return value;
}

/// The @p count bytes (4 at most) at @p index as a big-endian number.
inline std::uint32_t big(std::string_view bytes, std::size_t index, int count)
{
    std::uint32_t value = 0;
    for (int i = 0; i < count; ++i)
        value = value << 8 | at(bytes, index + std::size_t(i));
    return value;
}

/// Calls @p visit(type, data, whole) for each chunk of the PNG @p bytes
/// after its signature, where whole is the chunk with its length, type and
/// CRC, until @p visit returns false. The walk ends before IEND and at the
/// first chunk that is not whole.
template<typename Visit>
void forEachPngChunk(std::string_view bytes, Visit&& visit)
{
    for (std::size_t pos = 8; pos + 12 <= bytes.size();) {
        const std::size_t length = big(bytes, pos, 4);
        if (length > bytes.size() - pos - 12)
            return;
        const std::string_view type = bytes.substr(pos + 4, 4);
        if (type == "IEND" || !visit(type, bytes.substr(pos + 8, length), bytes.substr(pos, 12 + length)))
            return;
        pos += 12 + length;
    }
}

/// Calls @p visit(marker, segment) for each marker segment of the JPEG
/// @p bytes before its scan (SOS) or its end (EOI), where segment is what
/// follows the length, until @p visit returns false. Fill bytes and the
/// markers without a segment are passed over. The walk ends at the first
/// byte that is not a marker and at a segment that is not whole.
template<typename Visit>
void forEachJpegSegment(std::string_view bytes, Visit&& visit)
{
    std::size_t pos = 2;
    while (pos + 4 <= bytes.size() && at(bytes, pos) == 0xFF) {
        const std::uint8_t marker = at(bytes, pos + 1);
        if (marker == 0xFF) {
            ++pos; // a fill byte
            continue;
        }
        if (marker == 0xDA || marker == 0xD9)
            return;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        const std::size_t length = big(bytes, pos + 2, 2);
        if (length < 2 || length > bytes.size() - pos - 2 || !visit(marker, bytes.substr(pos + 4, length - 2)))
            return;
        pos += 2 + length;
    }
}

/// Probe::TooLarge or Probe::Image for a size from a header, with the size
/// in the outputs. Unknown for an empty side, with 0 in both.
Probe sized(std::uint64_t width, std::uint64_t height, int& outWidth, int& outHeight);

/// True, with @p error set, when @p probe is not an image that the decoder
/// of the format @p name ("PNG") takes.
bool refused(Probe probe, const char* name, std::string& error);

/// The message of a codec without the name of the function that gave it
/// ("tj3DecompressHeader(): ", "png_image_finish_read: "), which tells a
/// user nothing.
std::string libraryMessage(std::string_view text);

/// What decode() tells a decoder besides the file.
struct Options
{
    /// The colour space of the file is known: the converter makes its
    /// pixels sRGB, or they are sRGB already. A PNG then goes to libpng
    /// without its gAMA, cHRM, sRGB, iCCP and cICP chunks, so that no
    /// curve applies twice.
    bool colorKnown = false;
    PngFrames pngFrames = PngFrames::Animation;
};

// Each format: the size from its header, the colour space that it declares
// (called only for a file whose probe gives Probe::Image), and its decoder,
// which refuses what its probe refuses.

Probe probePng(std::string_view bytes, int& width, int& height);
ColorSpace pngColorSpace(std::string_view bytes);
bool decodePng(std::string_view bytes, const FrameSink& sink, const Options& options, std::string& error,
               std::string& warning);

Probe probeJpeg(std::string_view bytes, int& width, int& height);
ColorSpace jpegColorSpace(std::string_view bytes);
bool decodeJpeg(std::string_view bytes, const FrameSink& sink, const Options& options, std::string& error,
                std::string& warning);

Probe probeWebP(std::string_view bytes, int& width, int& height);
ColorSpace webpColorSpace(std::string_view bytes);
bool decodeWebP(std::string_view bytes, const FrameSink& sink, const Options& options, std::string& error,
                std::string& warning);

Probe probeGif(std::string_view bytes, int& width, int& height);
bool decodeGif(std::string_view bytes, const FrameSink& sink, const Options& options, std::string& error,
               std::string& warning);

Probe probeBmp(std::string_view bytes, int& width, int& height);
bool decodeBmp(std::string_view bytes, const FrameSink& sink, const Options& options, std::string& error,
               std::string& warning);

Probe probeTiff(std::string_view bytes, int& width, int& height);
ColorSpace tiffColorSpace(std::string_view bytes);
bool decodeTiff(std::string_view bytes, const FrameSink& sink, const Options& options, std::string& error,
                std::string& warning);

Probe probePnm(std::string_view bytes, int& width, int& height);
bool decodePnm(std::string_view bytes, const FrameSink& sink, const Options& options, std::string& error,
               std::string& warning);

} // namespace AfterglowImage::detail
