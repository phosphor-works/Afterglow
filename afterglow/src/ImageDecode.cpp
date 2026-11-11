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

#include <cstring>

namespace Afterglow {

namespace {

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
/// The daemon of sessions takes the same formats but WebP, by their
/// signatures (allowed() in mux/src/ImageDecoder.cpp): change both.
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
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty() || std::uint32_t(size.width()) > AfterglowProto::GraphicsMaxSide
        || std::uint32_t(size.height()) > AfterglowProto::GraphicsMaxSide
        || std::uint64_t(size.width()) * size.height() * 4 > AfterglowVt::GraphicsStore::DefaultQuota) return false;
    return toRgba(reader.read(), width, height, rgba);
}

} // namespace Afterglow
