// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowImage/Decode.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace AfterglowImage::detail {

/// The colour space that an image file declares: an ICC profile, or a PNG's
/// cICP chunk, which comes first (the PNG specification, third edition).
/// Neither: the pixels are sRGB.
struct ColorSpace
{
    std::string icc;
    bool cicp = false;
    std::uint8_t primaries = 0; ///< H.273 colour primaries
    std::uint8_t transfer = 0; ///< H.273 transfer characteristics
    std::uint8_t matrix = 0; ///< 0 for RGB
    bool fullRange = true;
};

/// The colour space of @p bytes, a file of @p format: PNG (cICP, iCCP), JPEG
/// (APP2 ICC_PROFILE segments) and WebP (ICCP). GIF and BMP give none.
ColorSpace colorSpaceOf(Format format, std::string_view bytes);

/// A PNG without the chunks that say how its samples relate to light (gAMA,
/// cHRM, sRGB, iCCP, cICP): the ones that libpng's simplified API applies
/// itself, so that the converter is the one place that does.
std::string withoutColorChunks(std::string_view png);

/// Converts RGBA with straight alpha from a colour space to sRGB, with Little
/// CMS: an RGB ICC profile, or a cICP of the primaries BT.709, BT.2020 and
/// Display P3 with the sRGB, linear, PQ or HLG transfer. PQ and HLG map
/// 203 cd/m², their reference white (BT.2408), to the white of sRGB, and
/// what is brighter to it. Any other space converts nothing (active() is
/// false), and the pixels stay as they are.
class SrgbConverter
{
public:
    explicit SrgbConverter(const ColorSpace& space);
    ~SrgbConverter();
    SrgbConverter(const SrgbConverter&) = delete;
    SrgbConverter& operator=(const SrgbConverter&) = delete;

    bool active() const noexcept { return m_transform != nullptr; }
    /// @p in to @p out, @p pixels of 4 bytes each. They may be the same.
    void convert(const unsigned char* in, unsigned char* out, std::size_t pixels) const;

private:
    void* m_transform = nullptr;
};

} // namespace AfterglowImage::detail
