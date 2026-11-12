// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageDecode.h"

#include <AfterglowProto/Graphics.h>
#include <AfterglowVt/Graphics.h>

#include <QBuffer>
#include <QByteArray>
#include <QByteArrayList>
#include <QImage>
#include <QImageReader>

#include <array>
#include <cmath>
#include <cstring>
#include <optional>

namespace Afterglow {

namespace {

std::uint32_t big32(std::string_view bytes, std::size_t at)
{
    return std::uint32_t(std::uint8_t(bytes[at])) << 24 | std::uint32_t(std::uint8_t(bytes[at + 1])) << 16
        | std::uint32_t(std::uint8_t(bytes[at + 2])) << 8 | std::uint8_t(bytes[at + 3]);
}

/// The gamma of a PNG's gAMA chunk, when it has one and no sRGB chunk that
/// overrides it (see below): the chunks before the image data. Qt keeps it as the colour space of the
/// image and leaves the samples as they are, where kitty, the decoders of
/// aglow and of sessions (libpng) and browsers apply it.
std::optional<double> pngGamma(std::string_view png)
{
    std::optional<double> gamma;
    bool seen = false;
    bool cicp = false;
    for (std::size_t at = 8; at + 12 <= png.size();) {
        const std::size_t length = big32(png, at);
        if (length > png.size() - at - 12) break;
        const std::string_view type = png.substr(at + 4, 4);
        // As libpng 1.6.59 reads the chunks that it takes (it also checks
        // their CRCs, which this does not): a valid sRGB chunk wins, and
        // both count only before the palette and the image data.
        if (type == "IDAT" || type == "PLTE") return gamma;
        // A cICP chunk keeps a gamma that a gAMA chunk gave before the sRGB
        // chunk (png_handle_sRGB), though libpng takes no gamma from it.
        if (type == "cICP" && length == 4) cicp = true;
        if (type == "sRGB" && length == 1 && std::uint8_t(png[at + 8]) <= 3 && !(cicp && gamma)) return std::nullopt;
        // The first valid one: libpng ignores a value past 31 bits, and the
        // gAMA chunks after one that it took, also one of 0, which is no gamma.
        if (type == "gAMA" && !seen && length == 4) {
            const std::uint32_t value = big32(png, at + 8);
            if (value <= 0x7FFFFFFFu) {
                seen = true;
                if (value != 0) gamma = value / 100000.0;
            }
        }
        at += 12 + length;
    }
    return gamma;
}

/// Applies the gamma @p gamma of a PNG to the colour samples of @p rgba, for
/// a display of gamma 2.2, as kitty does (png_set_gamma(2.2, gamma)).
void applyGamma(std::vector<std::uint8_t>& rgba, double gamma)
{
    // As libpng: a correction within 5% of none is not made
    // (png_gamma_significant, PNG_GAMMA_THRESHOLD_FIXED 5000).
    if (std::abs(gamma * 2.2 - 1) <= 0.05) return;
    std::array<std::uint8_t, 256> table{};
    for (int i = 0; i < 256; ++i) table[std::size_t(i)] = std::uint8_t(std::lround(255 * std::pow(i / 255.0, 1 / (gamma * 2.2))));
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = table[rgba[i]];
        rgba[i + 1] = table[rgba[i + 1]];
        rgba[i + 2] = table[rgba[i + 2]];
    }
}

bool toRgba(QImage img, std::uint32_t& width, std::uint32_t& height, std::vector<std::uint8_t>& rgba)
{
    if (img.isNull() || std::uint32_t(img.width()) > AfterglowProto::GraphicsMaxSide
        || std::uint32_t(img.height()) > AfterglowProto::GraphicsMaxSide
        || std::uint64_t(img.width()) * img.height() * 4 > AfterglowVt::GraphicsStore::DefaultQuota) return false;
    img.convertTo(QImage::Format_RGBA8888);
    if (img.isNull()) return false;
    width = std::uint32_t(img.width());
    height = std::uint32_t(img.height());
    rgba.resize(std::size_t(width) * height * 4);
    for (int y = 0; y < img.height(); ++y) {
        std::memcpy(rgba.data() + std::size_t(y) * width * 4, img.constScanLine(y), std::size_t(width) * 4);
    }
    return true;
}

/// The formats a program may send as an image. Other Qt image plugins (SVG,
/// TIFF, PSD, RAW…) are large parsers that remote output should not reach.
/// The daemon of sessions takes the same formats, by their signatures
/// (allowed() in mux/src/ImageDecoder.cpp): change both. WebP needs Qt's
/// image formats plugin, which a session does not.
bool imageFormatAllowed(const QByteArray& format)
{
    static const QByteArrayList allowed{"png", "jpeg", "jpg", "gif", "bmp", "webp"};
    return allowed.contains(format.toLower());
}

} // namespace

bool decodeBoundedImage(std::string_view data, const char* format, std::uint32_t& width, std::uint32_t& height,
                        std::vector<std::uint8_t>& rgba)
{
    if (data.size() > AfterglowVt::GraphicsStore::DefaultQuota) return false;
    QByteArray bytes = QByteArray::fromRawData(data.data(), qsizetype(data.size()));
    QBuffer input(&bytes);
    if (!input.open(QIODevice::ReadOnly)) return false;
    QByteArray detected(format);
    if (detected.isEmpty()) {
        detected = QImageReader::imageFormat(&input);
        if (!imageFormatAllowed(detected)) return false;
    }
    QImageReader reader(&input, detected);
    // Qt gives a BMP whose rows are top-down (a negative height in its
    // header) a negative height here, though it reads the image.
    QSize size = reader.size();
    size.setHeight(std::abs(size.height()));
    if (!size.isValid() || size.isEmpty() || std::uint32_t(size.width()) > AfterglowProto::GraphicsMaxSide
        || std::uint32_t(size.height()) > AfterglowProto::GraphicsMaxSide
        || std::uint64_t(size.width()) * size.height() * 4 > AfterglowVt::GraphicsStore::DefaultQuota) return false;
    if (!toRgba(reader.read(), width, height, rgba)) return false;
    if (detected.toLower() == "png") {
        if (const auto gamma = pngGamma(data)) applyGamma(rgba, *gamma);
    }
    return true;
}

} // namespace Afterglow
