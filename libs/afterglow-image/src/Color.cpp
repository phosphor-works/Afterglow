// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The colour space of an image file, and its conversion to sRGB with Little
// CMS. The files are read by the formats' own rules, apart from the decoders:
// PNG chunks, JPEG APP2 segments and WebP RIFF chunks.

#include "Color.h"
#include "Formats.h"

#include <lcms2.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

namespace AfterglowImage::detail {

namespace {

/// The largest ICC profile that a file may hold. Real ones are a few
/// kilobytes; a profile with large lookup tables can reach a megabyte.
constexpr std::size_t MaxIcc = std::size_t(4) << 20;

/// An iCCP chunk's profile: its name, a NUL, the compression method (0) and
/// the profile deflated with zlib.
std::string inflateIcc(std::string_view chunk)
{
    const std::size_t nul = chunk.find('\0');
    if (nul == std::string_view::npos || nul == 0 || nul > 79 || nul + 2 > chunk.size() || chunk[nul + 1] != 0) return {};
    const std::string_view deflated = chunk.substr(nul + 2);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) return {};
    std::string out;
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(deflated.data()));
    stream.avail_in = uInt(deflated.size());
    std::array<char, 16384> buffer{};
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = uInt(buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        out.append(buffer.data(), buffer.size() - stream.avail_out);
        if (out.size() > MaxIcc) result = Z_MEM_ERROR;
    }
    inflateEnd(&stream);
    return result == Z_STREAM_END ? out : std::string();
}

ColorSpace pngSpace(std::string_view bytes)
{
    ColorSpace space;
    // The chunks before the image data: the colour chunks come before IDAT.
    for (std::size_t pos = 8; pos + 12 <= bytes.size();) {
        const std::size_t length = big(bytes, pos, 4);
        if (length > bytes.size() - pos - 12) break;
        const std::string_view type = bytes.substr(pos + 4, 4);
        const std::string_view data = bytes.substr(pos + 8, length);
        if (type == "IDAT") break;
        if (type == "cICP" && length == 4) {
            space.cicp = true;
            space.primaries = at(data, 0);
            space.transfer = at(data, 1);
            space.matrix = at(data, 2);
            space.fullRange = at(data, 3) == 1;
        } else if (type == "iCCP" && space.icc.empty()) {
            space.icc = inflateIcc(data);
        }
        pos += 12 + length;
    }
    return space;
}

ColorSpace jpegSpace(std::string_view bytes)
{
    // APP2 segments "ICC_PROFILE\0", sequence number, count, then a part of
    // the profile; the parts go together in their order.
    constexpr std::string_view Tag("ICC_PROFILE\0", 12);
    std::map<int, std::string_view> parts;
    int count = 0;
    std::size_t pos = 2;
    while (pos + 4 <= bytes.size() && std::uint8_t(bytes[pos]) == 0xFF) {
        const std::uint8_t marker = at(bytes, pos + 1);
        if (marker == 0xFF) {
            ++pos; // a fill byte
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) break; // the scan, or the end
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        const std::size_t length = big(bytes, pos + 2, 2);
        if (length < 2 || pos + 2 + length > bytes.size()) break;
        const std::string_view segment = bytes.substr(pos + 4, length - 2);
        if (marker == 0xE2 && segment.size() > Tag.size() + 2 && segment.substr(0, Tag.size()) == Tag) {
            const int number = at(segment, Tag.size());
            const int of = at(segment, Tag.size() + 1);
            if (number >= 1 && number <= of && (count == 0 || count == of)) {
                count = of;
                parts.emplace(number, segment.substr(Tag.size() + 2));
            }
        }
        pos += 2 + length;
    }
    ColorSpace space;
    if (count == 0 || int(parts.size()) != count) return space;
    for (const auto& [number, part] : parts) {
        if (space.icc.size() + part.size() > MaxIcc) return {};
        space.icc.append(part);
    }
    return space;
}

ColorSpace webpSpace(std::string_view bytes)
{
    ColorSpace space;
    for (std::size_t pos = 12; pos + 8 <= bytes.size();) {
        const std::size_t length = little(bytes, pos + 4, 4);
        if (length > bytes.size() - pos - 8) break;
        if (bytes.substr(pos, 4) == "ICCP") {
            if (length <= MaxIcc) space.icc.assign(bytes.substr(pos + 8, length));
            break;
        }
        pos += 8 + length + (length & 1);
    }
    return space;
}

// H.273 transfer characteristics: the curve from a code value to light.
double pq(double v)
{
    // SMPTE ST 2084, in cd/m².
    constexpr double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32,
                     c3 = 2392.0 / 4096 * 32;
    const double e = std::pow(v, 1 / m2);
    return 10000 * std::pow(std::max(e - c1, 0.0) / (c2 - c3 * e), 1 / m1);
}

double hlg(double v)
{
    // BT.2100 inverse OETF, then the display's gamma of 1.2 pos a peak of
    // 1000 cd/m², per colour channel.
    constexpr double a = 0.17883277, b = 1 - 4 * a;
    const double c = 0.5 - a * std::log(4 * a);
    const double scene = v <= 0.5 ? v * v / 3 : (std::exp((v - c) / a) + b) / 12;
    return 1000 * std::pow(scene, 1.2);
}

/// Reference white (BT.2408) of PQ and HLG: the white of sRGB.
constexpr double ReferenceWhite = 203;

cmsToneCurve* tabulated(double (*nits)(double))
{
    std::vector<cmsFloat32Number> values(4096);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = cmsFloat32Number(std::min(1.0, nits(double(i) / double(values.size() - 1)) / ReferenceWhite));
    }
    return cmsBuildTabulatedToneCurveFloat(nullptr, cmsUInt32Number(values.size()), values.data());
}

cmsHPROFILE cicpProfile(const ColorSpace& s)
{
    // RGB samples of the full range only: the matrices of YCbCr and the
    // narrow range belong to video.
    if (s.matrix != 0 || !s.fullRange) return nullptr;
    cmsCIExyYTRIPLE primaries;
    switch (s.primaries) {
    case 1: primaries = {{0.64, 0.33, 1}, {0.30, 0.60, 1}, {0.15, 0.06, 1}}; break; // BT.709, sRGB
    case 9: primaries = {{0.708, 0.292, 1}, {0.170, 0.797, 1}, {0.131, 0.046, 1}}; break; // BT.2020
    case 12: primaries = {{0.680, 0.320, 1}, {0.265, 0.690, 1}, {0.150, 0.060, 1}}; break; // Display P3
    default: return nullptr;
    }
    cmsToneCurve* curve = nullptr;
    switch (s.transfer) {
    case 13: {
        // IEC 61966-2-1, as ICC parametric curve type 4.
        const cmsFloat64Number sRGB[5] = {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
        if (s.primaries == 1) return nullptr; // sRGB already
        curve = cmsBuildParametricToneCurve(nullptr, 4, sRGB);
        break;
    }
    case 8: curve = cmsBuildGamma(nullptr, 1.0); break;
    case 16: curve = tabulated(pq); break;
    case 18: curve = tabulated(hlg); break;
    default: return nullptr;
    }
    if (!curve) return nullptr;
    const cmsCIExyY d65{0.3127, 0.3290, 1};
    cmsToneCurve* curves[3] = {curve, curve, curve};
    cmsHPROFILE profile = cmsCreateRGBProfile(&d65, &primaries, curves);
    cmsFreeToneCurve(curve);
    return profile;
}

cmsHPROFILE iccProfile(const std::string& icc)
{
    cmsHPROFILE profile = cmsOpenProfileFromMem(icc.data(), cmsUInt32Number(icc.size()));
    if (!profile) return nullptr;
    // RGB data only: the decoders give RGB, also of a grey or CMYK file.
    const cmsProfileClassSignature kind = cmsGetDeviceClass(profile);
    if (cmsGetColorSpace(profile) != cmsSigRgbData
        || (kind != cmsSigInputClass && kind != cmsSigDisplayClass && kind != cmsSigOutputClass && kind != cmsSigColorSpaceClass)) {
        cmsCloseProfile(profile);
        return nullptr;
    }
    return profile;
}

} // namespace

ColorSpace colorSpaceOf(Format format, std::string_view bytes)
{
    switch (format) {
    case Format::Png: return pngSpace(bytes);
    case Format::Jpeg: return jpegSpace(bytes);
    case Format::WebP: return webpSpace(bytes);
    case Format::Gif:
    case Format::Bmp:
    case Format::Unknown: break;
    }
    return {};
}

std::string withoutColorChunks(std::string_view png)
{
    std::string out(png.substr(0, std::min<std::size_t>(8, png.size())));
    std::size_t pos = 8;
    while (pos + 12 <= png.size()) {
        const std::size_t length = big(png, pos, 4);
        if (length > png.size() - pos - 12) break;
        const std::string_view type = png.substr(pos + 4, 4);
        if (type != "gAMA" && type != "cHRM" && type != "sRGB" && type != "iCCP" && type != "cICP") {
            out.append(png.substr(pos, 12 + length));
        }
        pos += 12 + length;
    }
    // What does not make whole chunks goes as it is: libpng judges it.
    out.append(png.substr(std::min(pos, png.size())));
    return out;
}

SrgbConverter::SrgbConverter(const ColorSpace& space)
{
    cmsHPROFILE in = space.cicp ? cicpProfile(space) : !space.icc.empty() ? iccProfile(space.icc) : nullptr;
    if (!in) return;
    cmsHPROFILE out = cmsCreate_sRGBProfile();
    if (out) {
        m_transform = cmsCreateTransform(in, TYPE_RGBA_8, out, TYPE_RGBA_8, INTENT_PERCEPTUAL, cmsFLAGS_COPY_ALPHA);
        cmsCloseProfile(out);
    }
    cmsCloseProfile(in);
}

SrgbConverter::~SrgbConverter()
{
    if (m_transform) cmsDeleteTransform(static_cast<cmsHTRANSFORM>(m_transform));
}

void SrgbConverter::convert(const unsigned char* in, unsigned char* out, std::size_t pixels) const
{
    // Little CMS takes the same buffer for both when the formats have one
    // size, as these do.
    cmsDoTransform(static_cast<cmsHTRANSFORM>(m_transform), in, out, cmsUInt32Number(pixels));
}

} // namespace AfterglowImage::detail
