// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowImage/Decode.h>

#include <cstdint>
#include <string>
#include <string_view>

// The decoder of each format. decode() in Decode.cpp picks one by the
// signature, so each of them gets bytes that start with its own.
namespace AfterglowImage::detail {

inline std::uint8_t at(std::string_view bytes, std::size_t index) { return std::uint8_t(bytes[index]); }

/// The @p count bytes (4 at most) at @p index as a little-endian number.
inline std::uint32_t little(std::string_view bytes, std::size_t index, int count)
{
    std::uint32_t value = 0;
    for (int i = count; i-- > 0;) value = value << 8 | at(bytes, index + std::size_t(i));
    return value;
}

/// The @p count bytes (4 at most) at @p index as a big-endian number.
inline std::uint32_t big(std::string_view bytes, std::size_t index, int count)
{
    std::uint32_t value = 0;
    for (int i = 0; i < count; ++i) value = value << 8 | at(bytes, index + std::size_t(i));
    return value;
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

Probe probePng(std::string_view bytes, int& width, int& height);
bool decodePng(std::string_view bytes, const FrameSink& sink, std::string& error);

Probe probeJpeg(std::string_view bytes, int& width, int& height);
bool decodeJpeg(std::string_view bytes, const FrameSink& sink, std::string& error);

Probe probeWebP(std::string_view bytes, int& width, int& height);
bool decodeWebP(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning);

Probe probeGif(std::string_view bytes, int& width, int& height);
bool decodeGif(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning);

Probe probeBmp(std::string_view bytes, int& width, int& height);
bool decodeBmp(std::string_view bytes, const FrameSink& sink, std::string& error);

} // namespace AfterglowImage::detail
