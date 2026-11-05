// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Image.h"

#include <stb_image.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace Aglow {

namespace {

// stb_image refuses larger images itself; this keeps the size arithmetic in range.
constexpr int MaxSide = 1 << 15;

// One pass of a separable resample: rows of @p channels-byte pixels from
// @p from to @p to pixels along the axis. A triangle filter as wide as the
// scale averages every source pixel when shrinking and interpolates when growing.
void resampleAxis(const std::vector<float>& in, std::vector<float>& out, int from, int to, int lines, bool horizontal)
{
    const double scale = double(to) / double(from);
    const double support = scale < 1 ? 1 / scale : 1;
    out.assign(std::size_t(to) * std::size_t(lines) * 4, 0.0f);
    std::vector<double> weights;
    for (int d = 0; d < to; ++d) {
        const double center = (d + 0.5) / scale - 0.5;
        int first = std::max(0, int(std::floor(center - support)));
        const int last = std::min(from - 1, int(std::ceil(center + support)));
        weights.assign(std::size_t(last - first + 1), 0.0);
        double total = 0;
        for (int s = first; s <= last; ++s) {
            const double w = std::max(0.0, 1 - std::abs(s - center) / support);
            weights[std::size_t(s - first)] = w;
            total += w;
        }
        if (total <= 0) {
            const int nearest = std::clamp(int(std::lround(center)), 0, from - 1);
            weights.assign(1, 1.0);
            total = 1;
            first = nearest;
        }
        for (int line = 0; line < lines; ++line) {
            float acc[4] = { 0, 0, 0, 0 };
            for (std::size_t k = 0; k < weights.size(); ++k) {
                const int s = first + int(k);
                const std::size_t at = horizontal ? (std::size_t(line) * from + std::size_t(s)) * 4
                                                  : (std::size_t(s) * lines + std::size_t(line)) * 4;
                for (int c = 0; c < 4; ++c) {
                    acc[c] += float(weights[k] / total) * in[at + std::size_t(c)];
                }
            }
            const std::size_t at = horizontal ? (std::size_t(line) * to + std::size_t(d)) * 4
                                              : (std::size_t(d) * lines + std::size_t(line)) * 4;
            for (int c = 0; c < 4; ++c) {
                out[at + std::size_t(c)] = acc[c];
            }
        }
    }
}

} // namespace

bool isPng(std::string_view bytes) { return bytes.starts_with("\x89PNG\r\n\x1a\n"); }

bool imageSize(std::string_view bytes, int& width, int& height)
{
    int channels = 0;
    if (bytes.size() > INT_MAX) {
        return false;
    }
    return stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &width, &height, &channels) == 1;
}

std::optional<Image> decodeImage(std::string_view bytes, std::string& error)
{
    if (bytes.size() > INT_MAX) {
        error = "the file is too large";
        return std::nullopt;
    }
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int size = int(bytes.size());
    Image image;
    int channels = 0;
    if (bytes.starts_with("GIF8")) {
        int* delays = nullptr;
        int count = 0;
        stbi_uc* pixels = stbi_load_gif_from_memory(data, size, &delays, &image.width, &image.height, &count, &channels, 4);
        if (!pixels) {
            error = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
            return std::nullopt;
        }
        const std::size_t frame = std::size_t(image.width) * std::size_t(image.height) * 4;
        for (int i = 0; i < count; ++i) {
            // Browsers show a GIF delay under 20 ms as 100 ms, as kitty does.
            const int delay = delays ? delays[i] : 0;
            image.frames.push_back({ std::string(reinterpret_cast<const char*>(pixels) + frame * std::size_t(i), frame),
                                     delay < 20 ? 100 : delay });
        }
        stbi_image_free(pixels);
        stbi_image_free(delays);
    } else {
        stbi_uc* pixels = stbi_load_from_memory(data, size, &image.width, &image.height, &channels, 4);
        if (!pixels) {
            error = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
            return std::nullopt;
        }
        image.frames.push_back({ std::string(reinterpret_cast<const char*>(pixels),
                                              std::size_t(image.width) * std::size_t(image.height) * 4),
                                 0 });
        stbi_image_free(pixels);
    }
    if (image.width <= 0 || image.height <= 0 || image.width > MaxSide || image.height > MaxSide || image.frames.empty()) {
        error = "the image has no pixels, or too many";
        return std::nullopt;
    }
    return image;
}

void resizeImage(Image& image, int width, int height)
{
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == image.width && height == image.height) {
        return;
    }
    for (auto& frame : image.frames) {
        // Premultiplied while filtering, so that transparent pixels lend no colour to their neighbours.
        std::vector<float> pixels(frame.rgba.size());
        for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
            const float alpha = float(std::uint8_t(frame.rgba[i + 3])) / 255.0f;
            for (int c = 0; c < 3; ++c) {
                pixels[i + std::size_t(c)] = float(std::uint8_t(frame.rgba[i + std::size_t(c)])) * alpha;
            }
            pixels[i + 3] = alpha * 255.0f;
        }
        std::vector<float> across;
        resampleAxis(pixels, across, image.width, width, image.height, true);
        std::vector<float> down;
        resampleAxis(across, down, image.height, height, width, false);
        frame.rgba.assign(std::size_t(width) * std::size_t(height) * 4, '\0');
        for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
            const float alpha = std::clamp(down[i + 3], 0.0f, 255.0f);
            for (int c = 0; c < 3; ++c) {
                const float value = alpha > 0 ? down[i + std::size_t(c)] * 255.0f / alpha : 0.0f;
                frame.rgba[i + std::size_t(c)] = char(std::uint8_t(std::clamp(std::lround(value), 0L, 255L)));
            }
            frame.rgba[i + 3] = char(std::uint8_t(std::lround(alpha)));
        }
    }
    image.width = width;
    image.height = height;
}

void mirrorImage(Image& image, bool horizontal, bool vertical)
{
    const std::size_t stride = std::size_t(image.width) * 4;
    for (auto& frame : image.frames) {
        if (horizontal) {
            for (int y = 0; y < image.height; ++y) {
                char* row = frame.rgba.data() + std::size_t(y) * stride;
                for (int a = 0, b = image.width - 1; a < b; ++a, --b) {
                    std::swap_ranges(row + std::size_t(a) * 4, row + std::size_t(a) * 4 + 4, row + std::size_t(b) * 4);
                }
            }
        }
        if (vertical) {
            for (int a = 0, b = image.height - 1; a < b; ++a, --b) {
                std::swap_ranges(frame.rgba.data() + std::size_t(a) * stride, frame.rgba.data() + std::size_t(a + 1) * stride,
                                 frame.rgba.data() + std::size_t(b) * stride);
            }
        }
    }
}

void flattenImage(Image& image, std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    const std::uint8_t colour[3] = { red, green, blue };
    for (auto& frame : image.frames) {
        for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
            const unsigned alpha = std::uint8_t(frame.rgba[i + 3]);
            for (int c = 0; c < 3; ++c) {
                const unsigned value = std::uint8_t(frame.rgba[i + std::size_t(c)]);
                frame.rgba[i + std::size_t(c)] = char((value * alpha + colour[c] * (255 - alpha) + 127) / 255);
            }
            frame.rgba[i + 3] = char(255);
        }
    }
}

bool imageOpaque(const Image& image)
{
    for (const auto& frame : image.frames) {
        for (std::size_t i = 3; i < frame.rgba.size(); i += 4) {
            if (std::uint8_t(frame.rgba[i]) != 255) {
                return false;
            }
        }
    }
    return true;
}

} // namespace Aglow
