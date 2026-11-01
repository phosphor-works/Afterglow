// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ITerm2.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <vector>

namespace AfterglowVt::detail {

namespace {

constexpr double MaxSide = 10000;
constexpr double MaxPixels = 4096.0 * 4096.0;

/// A size argument in pixels; negative for auto.
double sizeArg(std::string_view spec, int cell, double total)
{
    if (spec.empty() || spec == "auto") {
        return -1;
    }
    double unit = cell;
    if (spec.ends_with("px")) {
        spec.remove_suffix(2);
        unit = 1;
    } else if (spec.ends_with('%')) {
        spec.remove_suffix(1);
        unit = total / 100.0;
    }
    double n = 0;
    const auto [ptr, ec] = std::from_chars(spec.data(), spec.data() + spec.size(), n);
    if (ec != std::errc{} || ptr != spec.data() + spec.size() || n <= 0) {
        return -1;
    }
    return n * unit;
}

} // namespace

void parseITerm2Args(std::string_view args, ITerm2File& out)
{
    std::size_t pos = 0;
    while (pos <= args.size()) {
        const auto semi = args.find(';', pos);
        const std::string_view pair = args.substr(pos, semi == std::string_view::npos ? semi : semi - pos);
        const auto eq = pair.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view key = pair.substr(0, eq);
            const std::string_view value = pair.substr(eq + 1);
            if (key == "name") {
                out.name = std::string(value.substr(0, 4096));
            } else if (key == "width") {
                out.width = std::string(value.substr(0, 32));
            } else if (key == "height") {
                out.height = std::string(value.substr(0, 32));
            } else if (key == "preserveAspectRatio") {
                out.preserveAspectRatio = value != "0";
            } else if (key == "inline") {
                out.inlineImage = value == "1";
            } else if (key == "doNotMoveCursor") {
                out.doNotMoveCursor = value == "1";
            } else if (key == "size") {
                std::from_chars(value.data(), value.data() + value.size(), out.size);
            }
        }
        if (semi == std::string_view::npos) {
            break;
        }
        pos = semi + 1;
    }
}

bool iterm2ImageSize(const ITerm2File& file, std::uint32_t naturalW, std::uint32_t naturalH, int cellW, int cellH,
                     int cols, int rows, std::uint32_t& width, std::uint32_t& height)
{
    if (naturalW == 0 || naturalH == 0 || cellW <= 0 || cellH <= 0) {
        return false;
    }
    const double totalW = double(cols) * cellW;
    const double totalH = double(rows) * cellH;
    double w = sizeArg(file.width, cellW, totalW);
    double h = sizeArg(file.height, cellH, totalH);
    const double aspect = double(naturalW) / double(naturalH);
    if (w < 0 && h < 0) {
        // Natural size, made to fit the terminal's width.
        w = naturalW;
        h = naturalH;
        if (w > totalW && totalW > 0) {
            w = totalW;
            h = w / aspect;
        }
    } else if (file.preserveAspectRatio) {
        if (w < 0) {
            w = h * aspect;
        } else if (h < 0) {
            h = w / aspect;
        } else {
            // Within the box, keeping the shape.
            const double fit = std::min(w / naturalW, h / naturalH);
            w = naturalW * fit;
            h = naturalH * fit;
        }
    } else {
        if (w < 0) {
            w = naturalW;
        }
        if (h < 0) {
            h = naturalH;
        }
    }
    if (file.preserveAspectRatio) {
        // One factor for both sides, so the limits keep the shape.
        const double shrink = std::min({1.0, MaxSide / w, MaxSide / h, std::sqrt(MaxPixels / (w * h))});
        w *= shrink;
        h *= shrink;
    } else {
        w = std::min(w, MaxSide);
        h = std::min(h, MaxSide);
        if (w * h > MaxPixels) {
            const double shrink = std::sqrt(MaxPixels / (w * h));
            w *= shrink;
            h *= shrink;
        }
    }
    width = std::uint32_t(std::max(1.0, std::round(w)));
    height = std::uint32_t(std::max(1.0, std::round(h)));
    return true;
}

namespace {

/// The source pixels one output pixel reads along one axis, with weights:
/// the covered span when shrinking (area average), the two nearest pixels
/// when enlarging (linear).
struct Taps
{
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    double weight[2] = {1, 1}; ///< for the linear case; the span weighs 1 each
};

Taps axisTaps(std::uint32_t out, double scale, std::uint32_t size)
{
    Taps t;
    if (scale > 1.0) {
        t.first = std::min(size - 1, std::uint32_t(out * scale));
        const auto last = std::min(size, std::uint32_t(std::ceil((out + 1) * scale)));
        t.count = std::max(1u, last - t.first);
        return t;
    }
    const double f = std::max(0.0, (out + 0.5) * scale - 0.5);
    t.first = std::min(size - 1, std::uint32_t(f));
    const double frac = t.first + 1 < size ? f - t.first : 0.0;
    t.count = t.first + 1 < size ? 2 : 1;
    t.weight[0] = 1 - frac;
    t.weight[1] = frac;
    return t;
}

} // namespace

ImageFrame scaleFrame(const ImageFrame& in, std::uint32_t width, std::uint32_t height)
{
    if (width == in.width && height == in.height) {
        return in;
    }
    ImageFrame out;
    out.width = width;
    out.height = height;
    out.rgba.assign(std::size_t(width) * height * 4, 0);
    const double sx = double(in.width) / width;
    const double sy = double(in.height) / height;
    // Each axis picks its own filter, so a shrink along one and an
    // enlargement along the other stay smooth on both.
    const bool spanX = sx > 1.0;
    const bool spanY = sy > 1.0;
    std::vector<Taps> columns(width);
    for (std::uint32_t x = 0; x < width; ++x) {
        columns[x] = axisTaps(x, sx, in.width);
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        const Taps ty = axisTaps(y, sy, in.height);
        for (std::uint32_t x = 0; x < width; ++x) {
            const Taps& tx = columns[x];
            double acc[4] = {0, 0, 0, 0};
            double total = 0;
            for (std::uint32_t j = 0; j < ty.count; ++j) {
                const double wy = spanY ? 1.0 : ty.weight[j];
                const std::uint8_t* row = &in.rgba[std::size_t(ty.first + j) * in.width * 4];
                for (std::uint32_t i = 0; i < tx.count; ++i) {
                    const double w = wy * (spanX ? 1.0 : tx.weight[i]);
                    const std::uint8_t* p = row + std::size_t(tx.first + i) * 4;
                    // Weighted by alpha, so transparent pixels lend no colour.
                    const double a = p[3] / 255.0 * w;
                    acc[0] += p[0] * a;
                    acc[1] += p[1] * a;
                    acc[2] += p[2] * a;
                    acc[3] += a;
                    total += w;
                }
            }
            std::uint8_t* d = &out.rgba[(std::size_t(y) * width + x) * 4];
            for (int c = 0; c < 3; ++c) {
                d[c] = std::uint8_t(acc[3] > 0 ? std::clamp(acc[c] / acc[3], 0.0, 255.0) : 0);
            }
            d[3] = std::uint8_t(std::lround(std::clamp(total > 0 ? acc[3] / total : 0.0, 0.0, 1.0) * 255.0));
        }
    }
    return out;
}

} // namespace AfterglowVt::detail
