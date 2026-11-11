// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Formats.h"

#include <algorithm>
#include <climits>

namespace AfterglowImage {

namespace detail {

Probe sized(std::uint64_t width, std::uint64_t height, int& outWidth, int& outHeight)
{
    outWidth = int(std::min<std::uint64_t>(width, INT_MAX));
    outHeight = int(std::min<std::uint64_t>(height, INT_MAX));
    if (width == 0 || height == 0) return Probe::Unknown;
    return width > std::uint64_t(MaxSide) || height > std::uint64_t(MaxSide) ? Probe::TooLarge : Probe::Image;
}

} // namespace detail

Format format(std::string_view bytes)
{
    if (bytes.starts_with("\x89PNG\r\n\x1a\n")) return Format::Png;
    if (bytes.starts_with("\xff\xd8\xff")) return Format::Jpeg;
    if (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a")) return Format::Gif;
    if (bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP") return Format::WebP;
    if (bytes.starts_with("BM")) return Format::Bmp;
    return Format::Unknown;
}

Probe probe(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    switch (format(bytes)) {
    case Format::Png: return detail::probePng(bytes, width, height);
    case Format::Jpeg: return detail::probeJpeg(bytes, width, height);
    case Format::Gif: return detail::probeGif(bytes, width, height);
    case Format::WebP: return detail::probeWebP(bytes, width, height);
    case Format::Bmp: return detail::probeBmp(bytes, width, height);
    case Format::Unknown: break;
    }
    return Probe::Unknown;
}

bool decode(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning)
{
    switch (format(bytes)) {
    case Format::Png: return detail::decodePng(bytes, sink, error);
    case Format::Jpeg: return detail::decodeJpeg(bytes, sink, error);
    case Format::Gif: return detail::decodeGif(bytes, sink, error, warning);
    case Format::WebP: return detail::decodeWebP(bytes, sink, error, warning);
    case Format::Bmp: return detail::decodeBmp(bytes, sink, error);
    case Format::Unknown: break;
    }
    error = "not an image that this decoder reads";
    return false;
}

} // namespace AfterglowImage
