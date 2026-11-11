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

/// Probe::TooLarge or Probe::Image for a size from a header, Unknown for an empty side.
Probe sized(std::uint64_t width, std::uint64_t height, int& outWidth, int& outHeight);

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
