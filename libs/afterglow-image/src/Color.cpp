// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// The conversion of an image's colour space to sRGB, with Little CMS. Each
// format reads the colour space that its files declare (Formats.h).

#include "Color.h"

#include <lcms2.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace AfterglowImage::detail {

namespace {

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
    // BT.2100 inverse OETF, then the display's gamma of 1.2 for a peak of
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
    if (s.matrix != 0 || !s.fullRange)
        return nullptr;
    cmsCIExyYTRIPLE primaries;
    switch (s.primaries) {
    case 1:
        primaries = {{0.64, 0.33, 1}, {0.30, 0.60, 1}, {0.15, 0.06, 1}};
        break; // BT.709, sRGB
    case 9:
        primaries = {{0.708, 0.292, 1}, {0.170, 0.797, 1}, {0.131, 0.046, 1}};
        break; // BT.2020
    case 12:
        primaries = {{0.680, 0.320, 1}, {0.265, 0.690, 1}, {0.150, 0.060, 1}};
        break; // Display P3
    default:
        return nullptr;
    }
    cmsToneCurve* curve = nullptr;
    switch (s.transfer) {
    case 13: {
        // IEC 61966-2-1, as ICC parametric curve type 4.
        const cmsFloat64Number sRGB[5] = {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
        curve = cmsBuildParametricToneCurve(nullptr, 4, sRGB);
        break;
    }
    case 8:
        curve = cmsBuildGamma(nullptr, 1.0);
        break;
    case 16:
        curve = tabulated(pq);
        break;
    case 18:
        curve = tabulated(hlg);
        break;
    default:
        return nullptr;
    }
    if (!curve)
        return nullptr;
    const cmsCIExyY d65{0.3127, 0.3290, 1};
    cmsToneCurve* curves[3] = {curve, curve, curve};
    cmsHPROFILE profile = cmsCreateRGBProfile(&d65, &primaries, curves);
    cmsFreeToneCurve(curve);
    return profile;
}

cmsHPROFILE iccProfile(const std::string& icc)
{
    cmsHPROFILE profile = cmsOpenProfileFromMem(icc.data(), cmsUInt32Number(icc.size()));
    if (!profile)
        return nullptr;
    // RGB data only: the decoders give RGB, also of a grey or CMYK file.
    const cmsProfileClassSignature kind = cmsGetDeviceClass(profile);
    if (cmsGetColorSpace(profile) != cmsSigRgbData
        || (kind != cmsSigInputClass && kind != cmsSigDisplayClass && kind != cmsSigOutputClass
            && kind != cmsSigColorSpaceClass)) {
        cmsCloseProfile(profile);
        return nullptr;
    }
    return profile;
}

/// A transform that moves no sample of 16 levels of each channel by more
/// than 1: the profile is sRGB, as many files embed it, and the pixels stay
/// as they are, without the time and the rounding of a conversion.
bool keepsSrgb(cmsHTRANSFORM transform)
{
    constexpr int Levels = 16;
    std::vector<unsigned char> in(std::size_t(Levels * Levels * Levels) * 4);
    for (std::size_t i = 0; i < in.size() / 4; ++i) {
        in[i * 4] = static_cast<unsigned char>(i % Levels * 17);
        in[i * 4 + 1] = static_cast<unsigned char>(i / Levels % Levels * 17);
        in[i * 4 + 2] = static_cast<unsigned char>(i / (Levels * Levels) * 17);
        in[i * 4 + 3] = 255;
    }
    std::vector<unsigned char> out(in.size());
    cmsDoTransform(transform, in.data(), out.data(), cmsUInt32Number(in.size() / 4));
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (std::abs(int(in[i]) - int(out[i])) > 1)
            return false;
    }
    return true;
}

} // namespace

SrgbConverter::SrgbConverter(const ColorSpace& space)
{
    // sRGB in cICP needs no conversion. A cICP that has no profile here
    // leaves the ICC profile, as a decoder that does not know the chunk
    // reads the file.
    if (space.cicp) {
        if (space.primaries == 1 && space.transfer == 13 && space.matrix == 0 && space.fullRange) {
            m_known = true;
            return;
        }
        if (cmsHPROFILE profile = cicpProfile(space))
            use(profile);
        if (m_known)
            return;
    }
    // An RGB profile on grey samples is an error of the file: libpng drops
    // it from a grey PNG, and so does this for each format.
    if (!space.icc.empty() && !space.grey) {
        if (cmsHPROFILE profile = iccProfile(space.icc))
            use(profile);
    }
}

void SrgbConverter::use(void* profile)
{
    cmsHPROFILE out = cmsCreate_sRGBProfile();
    cmsHTRANSFORM transform = out
        ? cmsCreateTransform(profile, TYPE_RGBA_8, out, TYPE_RGBA_8, INTENT_PERCEPTUAL, cmsFLAGS_COPY_ALPHA)
        : nullptr;
    if (out)
        cmsCloseProfile(out);
    cmsCloseProfile(profile);
    if (!transform)
        return;
    m_known = true;
    if (keepsSrgb(transform)) {
        cmsDeleteTransform(transform);
    } else {
        m_transform = transform;
    }
}

SrgbConverter::~SrgbConverter()
{
    if (m_transform)
        cmsDeleteTransform(static_cast<cmsHTRANSFORM>(m_transform));
}

void SrgbConverter::convert(const unsigned char* in, unsigned char* out, std::size_t pixels) const
{
    cmsDoTransform(static_cast<cmsHTRANSFORM>(m_transform), in, out, cmsUInt32Number(pixels));
}

} // namespace AfterglowImage::detail
