// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// JPEG through the TurboJPEG API of libjpeg-turbo.

#include "Formats.h"

#include <turbojpeg.h>

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
    // The markers up to the frame header, which holds the size.
    std::size_t i = 2;
    while (i + 4 <= bytes.size()) {
        if (at(bytes, i) != 0xFF) return Probe::Unknown;
        const std::uint8_t marker = at(bytes, i + 1);
        if (marker == 0xFF) {
            ++i; // a fill byte
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) return Probe::Unknown; // no frame before the image data
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        const std::size_t length = std::size_t(at(bytes, i + 2)) << 8 | at(bytes, i + 3);
        if (length < 2 || length > bytes.size() - i - 2) return Probe::Unknown;
        if (isFrameMarker(marker)) {
            if (length < 7) return Probe::Unknown;
            const std::uint64_t h = std::uint64_t(at(bytes, i + 5)) << 8 | at(bytes, i + 6);
            const std::uint64_t w = std::uint64_t(at(bytes, i + 7)) << 8 | at(bytes, i + 8);
            return sized(w, h, width, height);
        }
        i += 2 + length;
    }
    return Probe::Unknown;
}

bool decodeJpeg(std::string_view bytes, const FrameSink& sink, std::string& error)
{
    const std::unique_ptr<void, Handle> handle(tj3Init(TJINIT_DECOMPRESS));
    if (!handle) {
        error = "the JPEG decoder does not start";
        return false;
    }
    void* const h = handle.get();
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    tj3Set(h, TJPARAM_SCANLIMIT, MaxScans);
    tj3Set(h, TJPARAM_MAXPIXELS, MaxSide * MaxSide);
    if (tj3DecompressHeader(h, data, bytes.size()) != 0) {
        error = tj3GetErrorStr(h);
        return false;
    }
    const int width = tj3Get(h, TJPARAM_JPEGWIDTH);
    const int height = tj3Get(h, TJPARAM_JPEGHEIGHT);
    if (width <= 0 || height <= 0 || width > MaxSide || height > MaxSide) {
        error = "the JPEG has a side longer than " + std::to_string(MaxSide) + " pixels";
        return false;
    }
    if (tj3Get(h, TJPARAM_PRECISION) != 8) {
        error = "the JPEG has more than 8 bits for each sample";
        return false;
    }
    const int colourspace = tj3Get(h, TJPARAM_COLORSPACE);
    const bool cmyk = colourspace == TJCS_CMYK || colourspace == TJCS_YCCK;
    const std::size_t pixels = std::size_t(width) * std::size_t(height);
    std::vector<unsigned char> rgba(pixels * 4);
    // A warning (data that ends early, a bad marker) still gives the
    // picture, with grey where the data is missing, as browsers show it.
    if (tj3Decompress8(h, data, bytes.size(), rgba.data(), 0, cmyk ? TJPF_CMYK : TJPF_RGBA) != 0
        && tj3GetErrorCode(h) != TJERR_WARNING) {
        error = tj3GetErrorStr(h);
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
