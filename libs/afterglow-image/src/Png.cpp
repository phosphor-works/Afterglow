// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// PNG through libpng's simplified API, which takes every colour type and
// depth and gives 8-bit RGBA with straight alpha.

#include "Formats.h"

#include <png.h>

#include <memory>
#include <vector>

namespace AfterglowImage::detail {

namespace {

std::uint32_t big32(std::string_view bytes, std::size_t index)
{
    return std::uint32_t(at(bytes, index)) << 24 | std::uint32_t(at(bytes, index + 1)) << 16
        | std::uint32_t(at(bytes, index + 2)) << 8 | at(bytes, index + 3);
}

} // namespace

Probe probePng(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    // IHDR is the first chunk. Apple's CgBI PNG has a chunk before it, and
    // holds premultiplied BGR that libpng does not read.
    if (bytes.size() < 24 || bytes.substr(12, 4) != "IHDR") return Probe::Unknown;
    return sized(big32(bytes, 16), big32(bytes, 20), width, height);
}

bool decodePng(std::string_view bytes, const FrameSink& sink, std::string& error)
{
    int width = 0;
    int height = 0;
    if (probePng(bytes, width, height) != Probe::Image) {
        error = "the PNG has no valid header";
        return false;
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    // Frees what libpng holds, also after a failure, which frees it itself.
    const std::unique_ptr<png_image, void (*)(png_imagep)> guard(&image, png_image_free);
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) {
        error = image.message;
        return false;
    }
    // The header that libpng read is the one that probePng() checked.
    if (image.width != std::uint32_t(width) || image.height != std::uint32_t(height)) {
        error = "the PNG has no valid header";
        return false;
    }
    image.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) {
        error = image.message;
        return false;
    }
    sink(pixels.data(), width, height, 0);
    return true;
}

} // namespace AfterglowImage::detail
