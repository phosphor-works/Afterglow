// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// JPEG through the TurboJPEG API of libjpeg-turbo.

#include "Formats.h"

#include <turbojpeg.h>

#include <map>
#include <memory>
#include <vector>

namespace AfterglowImage::detail {

namespace {

// A progressive image walks the whole image for each scan: past this many
// scans, a file is made to take time, not to show a picture.
constexpr int MaxScans = 1000;

bool isFrameMarker(std::uint8_t marker)
{
    // SOF0 to SOF15, but DHT (C4), JPG (C8) and DAC (CC).
    return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

struct Handle
{
    void operator()(void* handle) const { tj3Destroy(handle); }
};

} // namespace

Probe probeJpeg(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    // The markers up to the frame header, which holds the size. No frame
    // before the image data: no size.
    Probe result = Probe::Unknown;
    forEachJpegSegment(bytes, [&](std::uint8_t marker, std::string_view segment) {
        if (!isFrameMarker(marker)) return true;
        if (segment.size() >= 5) result = sized(big(segment, 3, 2), big(segment, 1, 2), width, height);
        return false;
    });
    return result;
}

ColorSpace jpegColorSpace(std::string_view bytes)
{
    // APP2 segments "ICC_PROFILE\0", sequence number, count, then a part of
    // the profile; the parts go together in their order.
    constexpr std::string_view Tag("ICC_PROFILE\0", 12);
    std::map<int, std::string_view> parts;
    int count = 0;
    bool grey = false;
    forEachJpegSegment(bytes, [&](std::uint8_t marker, std::string_view segment) {
        if (isFrameMarker(marker) && segment.size() >= 6) grey = at(segment, 5) == 1; // one component
        if (marker == 0xE2 && segment.size() > Tag.size() + 2 && segment.substr(0, Tag.size()) == Tag) {
            const int number = at(segment, Tag.size());
            const int of = at(segment, Tag.size() + 1);
            if (number >= 1 && number <= of && (count == 0 || count == of)) {
                count = of;
                parts.emplace(number, segment.substr(Tag.size() + 2));
            }
        }
        return true;
    });
    ColorSpace space;
    space.grey = grey;
    if (count == 0 || int(parts.size()) != count) return space;
    for (const auto& [number, part] : parts) {
        if (space.icc.size() + part.size() > MaxIcc) {
            space.icc.clear();
            return space;
        }
        space.icc.append(part);
    }
    return space;
}

bool decodeJpeg(std::string_view bytes, const FrameSink& sink, const Options&, std::string& error, std::string&)
{
    int width = 0;
    int height = 0;
    if (refused(probeJpeg(bytes, width, height), "JPEG", error)) return false;
    const std::unique_ptr<void, Handle> handle(tj3Init(TJINIT_DECOMPRESS));
    if (!handle) {
        error = "the JPEG decoder does not start";
        return false;
    }
    void* const h = handle.get();
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    // Without its limits the decoder must not run.
    if (tj3Set(h, TJPARAM_SCANLIMIT, MaxScans) != 0 || tj3Set(h, TJPARAM_MAXPIXELS, MaxSide * MaxSide) != 0) {
        error = libraryMessage(tj3GetErrorStr(h));
        return false;
    }
    // A warning in the header (a JFIF version that is not 1) is no error:
    // the decoding below reads the header again and judges the whole file.
    // The error code is read before any other call, which clears it.
    if (tj3DecompressHeader(h, data, bytes.size()) != 0 && tj3GetErrorCode(h) != TJERR_WARNING) {
        error = libraryMessage(tj3GetErrorStr(h));
        return false;
    }
    // The header that libjpeg-turbo read is the one that probeJpeg() checked.
    if (tj3Get(h, TJPARAM_JPEGWIDTH) != width || tj3Get(h, TJPARAM_JPEGHEIGHT) != height) {
        error = "the JPEG has no valid header";
        return false;
    }
    if (tj3Get(h, TJPARAM_PRECISION) != 8) {
        error = "the JPEG does not have 8 bits for each sample";
        return false;
    }
    const int colourspace = tj3Get(h, TJPARAM_COLORSPACE);
    const bool cmyk = colourspace == TJCS_CMYK || colourspace == TJCS_YCCK;
    const std::size_t pixels = std::size_t(width) * std::size_t(height);
    std::vector<unsigned char> rgba(pixels * 4);
    // A warning (data that ends early, a bad marker) still gives the
    // picture, with grey where the data is missing, as browsers show it.
    // TurboJPEG also calls it a warning when a fatal error came after one,
    // and then rows are missing. In RGBA each decoded pixel has alpha 255,
    // and the buffer starts as zeros, so the last pixel tells whether the
    // last row came. CMYK has no such channel: there a warning is an error.
    if (tj3Decompress8(h, data, bytes.size(), rgba.data(), 0, cmyk ? TJPF_CMYK : TJPF_RGBA) != 0
        && (tj3GetErrorCode(h) != TJERR_WARNING || cmyk || rgba.back() != 255)) {
        error = libraryMessage(tj3GetErrorStr(h));
        return false;
    }
    if (cmyk) {
        // Adobe's inverted CMYK, as Qt reads it: each channel times K.
        for (std::size_t i = 0; i < pixels; ++i) {
            unsigned char* p = &rgba[i * 4];
            const unsigned k = p[3];
            p[0] = static_cast<unsigned char>(p[0] * k / 255);
            p[1] = static_cast<unsigned char>(p[1] * k / 255);
            p[2] = static_cast<unsigned char>(p[2] * k / 255);
            p[3] = 255;
        }
    }
    sink(rgba.data(), width, height, 0);
    return true;
}

} // namespace AfterglowImage::detail
