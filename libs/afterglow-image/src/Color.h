// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace AfterglowImage::detail {

/// The largest ICC profile that a file may hold. Real ones are a few
/// kilobytes; a profile with large lookup tables can reach a megabyte.
inline constexpr std::size_t MaxIcc = std::size_t(4) << 20;

/// The colour space that an image file declares: an ICC profile, or a PNG's
/// cICP chunk, which comes first (the PNG specification, third edition).
/// Neither: the pixels are sRGB. Each format reads its own (pngColorSpace()
/// and the others in Formats.h): PNG (cICP, iCCP), JPEG (APP2 ICC_PROFILE
/// segments), WebP (ICCP) and TIFF (the ICC profile field). GIF, BMP and
/// netPBM give none.
struct ColorSpace
{
    std::string icc;
    bool cicp = false;
    std::uint8_t primaries = 0; ///< H.273 colour primaries
    std::uint8_t transfer = 0; ///< H.273 transfer characteristics
    std::uint8_t matrix = 0; ///< 0 for RGB
    bool fullRange = true;
    /// The samples are grey: an RGB profile does not describe them.
    bool grey = false;
};

/// Converts RGBA with straight alpha from a colour space to sRGB, with Little
/// CMS: an RGB ICC profile of a colour image, or a cICP of the primaries
/// BT.709, BT.2020 and Display P3 with the sRGB, linear, PQ or HLG transfer.
/// PQ and HLG map 203 cd/m², their reference white (BT.2408), to the white
/// of sRGB, and what is brighter to it. A cICP that it does not take leaves
/// the ICC profile. A space that is sRGB already converts nothing, and is
/// known(). Any other space converts nothing and is not known: the pixels
/// stay as they are.
class SrgbConverter
{
public:
    explicit SrgbConverter(const ColorSpace& space);
    ~SrgbConverter();
    SrgbConverter(const SrgbConverter&) = delete;
    SrgbConverter& operator=(const SrgbConverter&) = delete;

    /// The pixels change: convert() has work to do.
    bool active() const noexcept
    {
        return m_transform != nullptr;
    }
    /// The colour space is one that the converter takes: after convert(),
    /// or as they are, the pixels are sRGB.
    bool known() const noexcept
    {
        return m_known;
    }
    /// @p in to @p out, @p pixels of 4 bytes each.
    void convert(const unsigned char* in, unsigned char* out, std::size_t pixels) const;

private:
    /// Takes @p profile (a cmsHPROFILE), and closes it.
    void use(void* profile);

    void* m_transform = nullptr;
    bool m_known = false;
};

} // namespace AfterglowImage::detail
