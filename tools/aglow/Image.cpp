// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Image.h"

#include <stb_image.h>

#include <AfterglowProto/Graphics.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace Aglow {

namespace {

// The weights of a separable resample along one axis, from @p from to @p to
// pixels. A triangle filter as wide as the scale averages every source pixel
// when shrinking and interpolates when growing.
struct Taps
{
    int first = 0;
    std::vector<float> weights;
};

std::vector<Taps> taps(int from, int to)
{
    const double scale = double(to) / double(from);
    const double support = scale < 1 ? 1 / scale : 1;
    std::vector<Taps> out(static_cast<std::size_t>(to));
    for (int d = 0; d < to; ++d) {
        const double center = (d + 0.5) / scale - 0.5;
        auto& tap = out[std::size_t(d)];
        tap.first = std::max(0, int(std::floor(center - support)));
        const int last = std::min(from - 1, int(std::ceil(center + support)));
        double total = 0;
        for (int s = tap.first; s <= last; ++s) {
            const double w = std::max(0.0, 1 - std::abs(s - center) / support);
            tap.weights.push_back(float(w));
            total += w;
        }
        if (total <= 0) {
            tap.first = std::clamp(int(std::lround(center)), 0, from - 1);
            tap.weights.assign(1, 1.0f);
            total = 1;
        }
        for (auto& w : tap.weights) {
            w = float(w / total);
        }
    }
    return out;
}

} // namespace

bool isPng(std::string_view bytes) { return bytes.starts_with("\x89PNG\r\n\x1a\n"); }

int gifFrames(std::string_view bytes)
{
    if (bytes.size() < 13 || !(bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a"))) {
        return 0;
    }
    auto byte = [&](std::size_t at) { return std::uint8_t(bytes[at]); };
    std::size_t at = 13;
    if (byte(10) & 0x80) {
        at += 3u << ((byte(10) & 7) + 1);
    }
    // Sub-blocks: a length byte and its data, up to a length of 0.
    auto skipBlocks = [&] {
        while (at < bytes.size()) {
            const auto length = byte(at++);
            if (length == 0) {
                return true;
            }
            at += length;
        }
        return false;
    };
    int frames = 0;
    while (at < bytes.size()) {
        const auto kind = byte(at++);
        if (kind == 0x21) {
            ++at; // the label
            if (!skipBlocks()) {
                break;
            }
        } else if (kind == 0x2C) {
            if (at + 10 > bytes.size()) {
                break;
            }
            const auto flags = byte(at + 8);
            at += 9;
            if (flags & 0x80) {
                at += 3u << ((flags & 7) + 1);
            }
            ++at; // the LZW code size
            ++frames;
            if (!skipBlocks()) {
                break;
            }
        } else {
            break; // the trailer, or data that is not a block
        }
    }
    return frames;
}

bool imageSize(std::string_view bytes, int& width, int& height)
{
    int channels = 0;
    if (bytes.size() > INT_MAX) {
        return false;
    }
    return stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &width, &height, &channels) == 1;
}

std::string Image::frame(int index) const
{
    const std::size_t size = std::size_t(width) * std::size_t(height) * 4;
    return std::string(reinterpret_cast<const char*>(m_pixels.get()) + size * std::size_t(index), size);
}

std::optional<Image> decodeImage(std::string_view bytes, std::size_t budget, std::string& error)
{
    Image image;
    if (bytes.size() > INT_MAX || !imageSize(bytes, image.width, image.height)) {
        error = "not an image that aglow can read";
        return std::nullopt;
    }
    // Checked before any pixel is decoded: a small file can name a huge image.
    const std::uint64_t frameBytes = std::uint64_t(image.width) * std::uint64_t(image.height) * 4;
    if (image.width <= 0 || image.height <= 0 || std::uint32_t(image.width) > AfterglowProto::GraphicsMaxSide
        || std::uint32_t(image.height) > AfterglowProto::GraphicsMaxSide || frameBytes > budget) {
        error = "the image is " + std::to_string(image.width) + "x" + std::to_string(image.height)
            + " pixels: more than a terminal takes";
        return std::nullopt;
    }
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int size = int(bytes.size());
    int channels = 0;
    int width = 0;
    int height = 0;
    stbi_uc* pixels = nullptr;
    const int frames = gifFrames(bytes);
    // An animation whose frames do not all fit shows its first frame only.
    if (frames > 1 && std::uint64_t(frames) * frameBytes <= budget) {
        int* delays = nullptr;
        int count = 0;
        pixels = stbi_load_gif_from_memory(data, size, &delays, &width, &height, &count, &channels, 4);
        for (int i = 0; pixels && i < count; ++i) {
            // Browsers show a GIF delay under 20 ms as 100 ms, as kitty does.
            const int delay = delays ? delays[i] : 0;
            image.m_delays.push_back(delay < 20 ? 100 : delay);
        }
        stbi_image_free(delays);
    } else {
        pixels = stbi_load_from_memory(data, size, &width, &height, &channels, 4);
        image.m_delays.push_back(0);
    }
    if (!pixels) {
        error = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
        return std::nullopt;
    }
    image.m_pixels.reset(pixels, [](unsigned char* p) { stbi_image_free(p); });
    if (width != image.width || height != image.height || image.m_delays.empty()) {
        error = "the image is not what its header says";
        return std::nullopt;
    }
    return image;
}

std::string resizeFrame(const std::string& rgba, int width, int height, int newWidth, int newHeight)
{
    newWidth = std::max(1, newWidth);
    newHeight = std::max(1, newHeight);
    if (newWidth == width && newHeight == height) {
        return rgba;
    }
    // Premultiplied while filtering, so that transparent pixels lend no
    // colour to their neighbours. Across first, from the bytes themselves:
    // no copy of the frame at its full size.
    const auto across = taps(width, newWidth);
    std::vector<float> middle(std::size_t(newWidth) * std::size_t(height) * 4);
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const std::uint8_t*>(rgba.data()) + std::size_t(y) * std::size_t(width) * 4;
        for (int x = 0; x < newWidth; ++x) {
            float acc[4] = { 0, 0, 0, 0 };
            const auto& tap = across[std::size_t(x)];
            for (std::size_t k = 0; k < tap.weights.size(); ++k) {
                const auto* px = row + std::size_t(tap.first + int(k)) * 4;
                const float alpha = px[3] / 255.0f;
                for (int c = 0; c < 3; ++c) {
                    acc[c] += tap.weights[k] * px[c] * alpha;
                }
                acc[3] += tap.weights[k] * px[3];
            }
            std::copy(std::begin(acc), std::end(acc), middle.begin() + std::ptrdiff_t((std::size_t(y) * std::size_t(newWidth) + std::size_t(x)) * 4));
        }
    }
    const auto down = taps(height, newHeight);
    std::string out(std::size_t(newWidth) * std::size_t(newHeight) * 4, '\0');
    for (int y = 0; y < newHeight; ++y) {
        const auto& tap = down[std::size_t(y)];
        for (int x = 0; x < newWidth; ++x) {
            float acc[4] = { 0, 0, 0, 0 };
            for (std::size_t k = 0; k < tap.weights.size(); ++k) {
                const float* px = middle.data() + (std::size_t(tap.first + int(k)) * std::size_t(newWidth) + std::size_t(x)) * 4;
                for (int c = 0; c < 4; ++c) {
                    acc[c] += tap.weights[k] * px[c];
                }
            }
            const std::size_t at = (std::size_t(y) * std::size_t(newWidth) + std::size_t(x)) * 4;
            const float alpha = std::clamp(acc[3], 0.0f, 255.0f);
            for (int c = 0; c < 3; ++c) {
                const float value = alpha > 0 ? acc[c] * 255.0f / alpha : 0.0f;
                out[at + std::size_t(c)] = char(std::uint8_t(std::clamp(std::lround(value), 0L, 255L)));
            }
            out[at + 3] = char(std::uint8_t(std::lround(alpha)));
        }
    }
    return out;
}

void mirrorFrame(std::string& rgba, int width, int height, bool horizontal, bool vertical)
{
    const std::size_t stride = std::size_t(width) * 4;
    if (horizontal) {
        for (int y = 0; y < height; ++y) {
            char* row = rgba.data() + std::size_t(y) * stride;
            for (int a = 0, b = width - 1; a < b; ++a, --b) {
                std::swap_ranges(row + std::size_t(a) * 4, row + std::size_t(a) * 4 + 4, row + std::size_t(b) * 4);
            }
        }
    }
    if (vertical) {
        for (int a = 0, b = height - 1; a < b; ++a, --b) {
            std::swap_ranges(rgba.data() + std::size_t(a) * stride, rgba.data() + std::size_t(a + 1) * stride,
                             rgba.data() + std::size_t(b) * stride);
        }
    }
}

void flattenFrame(std::string& rgba, std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    const std::uint8_t colour[3] = { red, green, blue };
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const unsigned alpha = std::uint8_t(rgba[i + 3]);
        for (int c = 0; c < 3; ++c) {
            const unsigned value = std::uint8_t(rgba[i + std::size_t(c)]);
            rgba[i + std::size_t(c)] = char((value * alpha + colour[c] * (255 - alpha) + 127) / 255);
        }
        rgba[i + 3] = char(255);
    }
}

bool frameOpaque(const std::string& rgba)
{
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        if (std::uint8_t(rgba[i]) != 255) {
            return false;
        }
    }
    return true;
}

std::string dropAlpha(const std::string& rgba)
{
    std::string rgb;
    rgb.reserve(rgba.size() / 4 * 3);
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        rgb.append(rgba, i, 3);
    }
    return rgb;
}

} // namespace Aglow
