// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ITerm2.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace AfterglowVt::detail {

namespace {

constexpr double MaxSide = 10000;
constexpr double MaxPixels = 4096.0 * 4096.0;

/// A size argument in pixels; negative for auto.
double sizeArg(std::string_view spec, int cell, int total)
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
    double w = sizeArg(file.width, cellW, int(totalW));
    double h = sizeArg(file.height, cellH, int(totalH));
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
    w = std::min(w, MaxSide);
    h = std::min(h, MaxSide);
    if (w * h > MaxPixels) {
        const double shrink = std::sqrt(MaxPixels / (w * h));
        w *= shrink;
        h *= shrink;
    }
    width = std::uint32_t(std::max(1.0, std::round(w)));
    height = std::uint32_t(std::max(1.0, std::round(h)));
    return true;
}

ImageFrame scaleFrame(const ImageFrame& in, std::uint32_t width, std::uint32_t height)
{
    if (width == in.width && height == in.height) {
        return in;
    }
    ImageFrame out;
    out.width = width;
    out.height = height;
    out.gapMs = in.gapMs;
    out.rgba.assign(std::size_t(width) * height * 4, 0);
    const double sx = double(in.width) / width;
    const double sy = double(in.height) / height;
    auto src = [&](std::uint32_t x, std::uint32_t y) { return &in.rgba[(std::size_t(y) * in.width + x) * 4]; };
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            double acc[4] = {0, 0, 0, 0};
            if (sx > 1.0 || sy > 1.0) {
                // Average the source pixels this one covers, weighted by alpha.
                const auto x0 = std::uint32_t(x * sx);
                const auto x1 = std::max(x0 + 1, std::min(in.width, std::uint32_t(std::ceil((x + 1) * sx))));
                const auto y0 = std::uint32_t(y * sy);
                const auto y1 = std::max(y0 + 1, std::min(in.height, std::uint32_t(std::ceil((y + 1) * sy))));
                double n = 0;
                for (std::uint32_t yy = y0; yy < y1; ++yy) {
                    for (std::uint32_t xx = x0; xx < x1; ++xx) {
                        const std::uint8_t* p = src(xx, yy);
                        const double a = p[3] / 255.0;
                        acc[0] += p[0] * a;
                        acc[1] += p[1] * a;
                        acc[2] += p[2] * a;
                        acc[3] += a;
                        n += 1;
                    }
                }
                const double alpha = acc[3] / n;
                std::uint8_t* d = &out.rgba[(std::size_t(y) * width + x) * 4];
                for (int c = 0; c < 3; ++c) {
                    d[c] = std::uint8_t(acc[3] > 0 ? std::clamp(acc[c] / acc[3], 0.0, 255.0) : 0);
                }
                d[3] = std::uint8_t(std::lround(alpha * 255.0));
            } else {
                // Bilinear between the four nearest source pixels.
                const double fx = std::max(0.0, (x + 0.5) * sx - 0.5);
                const double fy = std::max(0.0, (y + 0.5) * sy - 0.5);
                const auto ix = std::min(in.width - 1, std::uint32_t(fx));
                const auto iy = std::min(in.height - 1, std::uint32_t(fy));
                const auto jx = std::min(in.width - 1, ix + 1);
                const auto jy = std::min(in.height - 1, iy + 1);
                const double tx = fx - ix;
                const double ty = fy - iy;
                const std::uint8_t* q[4] = {src(ix, iy), src(jx, iy), src(ix, jy), src(jx, jy)};
                const double wgt[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
                for (int k = 0; k < 4; ++k) {
                    const double a = q[k][3] / 255.0 * wgt[k];
                    acc[0] += q[k][0] * a;
                    acc[1] += q[k][1] * a;
                    acc[2] += q[k][2] * a;
                    acc[3] += a;
                }
                std::uint8_t* d = &out.rgba[(std::size_t(y) * width + x) * 4];
                for (int c = 0; c < 3; ++c) {
                    d[c] = std::uint8_t(acc[3] > 0 ? std::clamp(acc[c] / acc[3], 0.0, 255.0) : 0);
                }
                d[3] = std::uint8_t(std::lround(std::clamp(acc[3], 0.0, 1.0) * 255.0));
            }
        }
    }
    return out;
}

} // namespace AfterglowVt::detail
