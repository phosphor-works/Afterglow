// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// PNG through libpng's simplified API, which takes every colour type and
// depth and gives 8-bit RGBA with straight alpha.

#include "Formats.h"

#include <png.h>

#include <memory>
#include <vector>

namespace AfterglowImage::detail {

Probe probePng(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    // IHDR is the first chunk. Apple's CgBI PNG has a chunk before it, and
    // holds premultiplied BGR that libpng does not read.
    if (bytes.size() < 24 || bytes.substr(12, 4) != "IHDR") return Probe::Unknown;
    return sized(big(bytes, 16, 4), big(bytes, 20, 4), width, height);
}

bool decodePng(std::string_view bytes, const FrameSink& sink, std::string& error)
{
    int width = 0;
    int height = 0;
    if (refused(probePng(bytes, width, height), "PNG", error)) return false;
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    // Frees what libpng holds, also after a failure, which frees it itself.
    const std::unique_ptr<png_image, void (*)(png_imagep)> guard(&image, png_image_free);
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) {
        error = libraryMessage(image.message);
        return false;
    }
    // The header that libpng read is the one that probePng() checked.
    if (image.width != std::uint32_t(width) || image.height != std::uint32_t(height)) {
        error = "the PNG has no valid header";
        return false;
    }
    image.format = PNG_FORMAT_RGBA;
    // Samples of 16 bits are sRGB like those of 8 bits, as kitty and Qt take
    // them: without this flag libpng takes them as linear light and makes a
    // file without a gAMA chunk brighter. A gAMA chunk still applies, as in
    // kitty. The flag is set after the header is read, which sets the flags.
    image.flags |= PNG_IMAGE_FLAG_16BIT_sRGB;
    std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) {
        error = libraryMessage(image.message);
        return false;
    }
    sink(pixels.data(), width, height, 0);
    return true;
}

} // namespace AfterglowImage::detail
