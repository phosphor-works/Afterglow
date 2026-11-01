// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Sixel.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace AfterglowVt {

namespace {

/// Pixel writes one image may make. '$' repaints a band from its start, so
/// without a limit a short string could repaint the same pixels forever.
/// Encoders write each pixel once per colour pass that sets it.
constexpr std::size_t MaxPaint = 4 * SixelMaxPixels;

std::uint32_t percentRgb(int r, int g, int b)
{
    auto c = [](int v) { return std::uint32_t(std::lround(std::clamp(v, 0, 100) * 255.0 / 100.0)); };
    return (c(r) << 16) | (c(g) << 8) | c(b);
}

/// DEC HLS: hue 0-360 with blue at 0, lightness and saturation 0-100.
std::uint32_t hlsRgb(int h, int l, int s)
{
    const double hue = std::fmod(double(h) + 240.0, 360.0); // DEC puts blue at 0
    const double light = std::clamp(l, 0, 100) / 100.0;
    const double sat = std::clamp(s, 0, 100) / 100.0;
    const double c = (1.0 - std::abs(2.0 * light - 1.0)) * sat;
    const double x = c * (1.0 - std::abs(std::fmod(hue / 60.0, 2.0) - 1.0));
    const double m = light - c / 2.0;
    double r = 0, g = 0, b = 0;
    if (hue < 60) {
        r = c, g = x;
    } else if (hue < 120) {
        r = x, g = c;
    } else if (hue < 180) {
        g = c, b = x;
    } else if (hue < 240) {
        g = x, b = c;
    } else if (hue < 300) {
        r = x, b = c;
    } else {
        r = c, b = x;
    }
    auto v = [m](double ch) { return std::uint32_t(std::lround(std::clamp(ch + m, 0.0, 1.0) * 255.0)); };
    return (v(r) << 16) | (v(g) << 8) | v(b);
}

/// Up to @p max decimal parameters separated by ';' starting at @p i.
int readParams(std::string_view s, std::size_t& i, int* out, int max)
{
    int count = 0;
    int value = 0;
    bool any = false;
    while (i < s.size()) {
        const char c = s[i];
        if (c >= '0' && c <= '9') {
            value = std::min(value * 10 + (c - '0'), 1 << 20);
            any = true;
        } else if (c == ';') {
            if (count < max) {
                out[count] = value;
            }
            ++count;
            value = 0;
            any = false;
        } else {
            break;
        }
        ++i;
    }
    if (any || count > 0) {
        if (count < max) {
            out[count] = value;
        }
        ++count;
    }
    return std::min(count, max);
}

} // namespace

SixelPalette defaultSixelPalette()
{
    SixelPalette p{};
    static constexpr int Vt340[16][3] = {
        {0, 0, 0}, {20, 20, 80}, {80, 13, 13}, {20, 80, 20}, {80, 20, 80}, {20, 80, 80}, {80, 80, 20}, {53, 53, 53},
        {26, 26, 26}, {33, 33, 60}, {60, 26, 26}, {33, 60, 33}, {60, 33, 60}, {33, 60, 60}, {60, 60, 33}, {80, 80, 80},
    };
    for (int i = 0; i < 16; ++i) {
        p[std::size_t(i)] = percentRgb(Vt340[i][0], Vt340[i][1], Vt340[i][2]);
    }
    return p;
}

bool decodeSixel(std::string_view data, int p2, std::uint32_t background, SixelPalette& palette, ImageFrame& out)
{
    // Pixels as palette-resolved 0xAARRGGBB; 0 is unpainted.
    std::vector<std::uint32_t> pixels;
    int allocW = 0;
    int allocH = 0;
    int usedW = 0;
    int usedH = 0;
    int rasterW = 0;
    int rasterH = 0;
    // Grows the pixel store to at least w x h. Only the side that needs it
    // grows, doubled to amortise the copies; the exact size is the fallback
    // when the doubled one would pass the pixel cap.
    auto ensure = [&](int w, int h) {
        if (w <= allocW && h <= allocH) {
            return true;
        }
        if (w > SixelMaxSide || h > SixelMaxSide) {
            return false;
        }
        const int needW = std::max(w, allocW);
        const int needH = std::max(h, allocH);
        if (std::size_t(needW) * std::size_t(needH) > SixelMaxPixels) {
            return false;
        }
        int nw = w > allocW ? std::min(SixelMaxSide, std::max(w, allocW * 2)) : allocW;
        int nh = h > allocH ? std::min(SixelMaxSide, std::max(h, allocH * 2)) : allocH;
        if (std::size_t(nw) * std::size_t(nh) > SixelMaxPixels) {
            nw = needW;
            nh = needH;
        }
        std::vector<std::uint32_t> grown(std::size_t(nw) * std::size_t(nh), 0);
        for (int y = 0; y < allocH; ++y) {
            std::copy_n(pixels.begin() + std::ptrdiff_t(y) * allocW, allocW, grown.begin() + std::ptrdiff_t(y) * nw);
        }
        pixels = std::move(grown);
        allocW = nw;
        allocH = nh;
        return true;
    };
    int x = 0;
    int band = 0;
    std::uint32_t color = palette[0] | 0xFF000000u;
    std::size_t painted = 0;
    // Paints the set bits of one sixel over n columns from x. A band needs
    // only the rows up to its highest set bit.
    auto paint = [&](int bits, int n) {
        const int rows = std::bit_width(unsigned(bits));
        if (band + rows > SixelMaxSide) {
            return false;
        }
        painted += std::size_t(std::popcount(unsigned(bits))) * std::size_t(n);
        if (painted > MaxPaint || !ensure(x + n, band + rows)) {
            return false;
        }
        for (int k = 0; k < rows; ++k) {
            if (bits & (1 << k)) {
                std::uint32_t* row = &pixels[std::size_t(band + k) * std::size_t(allocW)];
                std::fill(row + x, row + x + n, color);
            }
        }
        usedH = std::max(usedH, band + rows);
        usedW = std::max(usedW, x + n);
        return true;
    };

    std::size_t i = 0;
    while (i < data.size()) {
        const char c = data[i];
        if (c >= '?' && c <= '~') {
            if (x >= SixelMaxSide) return false;
            const int bits = c - '?';
            if (bits != 0 && !paint(bits, 1)) {
                return false;
            }
            ++x;
            ++i;
        } else if (c == '!') {
            // Repeat: ! Pn <sixel>
            ++i;
            int n = 1;
            readParams(data, i, &n, 1);
            if (i < data.size() && data[i] >= '?' && data[i] <= '~') {
                const int bits = data[i] - '?';
                n = std::max(n, 1);
                if (n > SixelMaxSide - x) return false;
                if (bits != 0 && !paint(bits, n)) {
                    return false;
                }
                x += n;
                ++i;
            }
        } else if (c == '#') {
            // Colour: # Pc (select) or # Pc ; Pu ; Px ; Py ; Pz (define and select).
            ++i;
            int params[5] = {0, 0, 0, 0, 0};
            const int n = readParams(data, i, params, 5);
            const int reg = std::clamp(params[0], 0, int(palette.size()) - 1);
            if (n >= 5) {
                palette[std::size_t(reg)] = params[1] == 1 ? hlsRgb(params[2], params[3], params[4])
                                                           : percentRgb(params[2], params[3], params[4]);
            }
            color = palette[std::size_t(reg)] | 0xFF000000u;
        } else if (c == '"') {
            // Raster attributes: " Pan ; Pad ; Ph ; Pv. The size only sets
            // the image's extent: nothing is allocated until pixels arrive.
            ++i;
            int params[4] = {1, 1, 0, 0};
            readParams(data, i, params, 4);
            rasterW = std::clamp(params[2], 0, SixelMaxSide);
            rasterH = std::clamp(params[3], 0, SixelMaxSide);
            if (std::size_t(rasterW) * std::size_t(rasterH) > SixelMaxPixels) {
                return false;
            }
        } else if (c == '$') {
            x = 0;
            ++i;
        } else if (c == '-') {
            // One line feed past the last paintable band is harmless (many
            // encoders end with one); more would only run up the counter.
            x = 0;
            if (band >= SixelMaxSide) return false;
            band += 6;
            ++i;
        } else {
            ++i; // anything else is ignored
        }
    }

    const int width = std::max(usedW, rasterW);
    const int height = std::max(usedH, rasterH);
    if (width <= 0 || height <= 0 || std::size_t(width) * std::size_t(height) > SixelMaxPixels) {
        return false;
    }
    out.width = std::uint32_t(width);
    out.height = std::uint32_t(height);
    // Zeroed is transparent: with P2 = 1 only the painted area needs a pass.
    out.rgba.assign(std::size_t(width) * std::size_t(height) * 4, 0);
    const bool transparent = p2 == 1;
    const std::uint32_t fill = background | 0xFF000000u;
    const int rows = transparent ? std::min(height, usedH) : height;
    const int cols = transparent ? std::min(width, usedW) : width;
    for (int y = 0; y < rows; ++y) {
        for (int px = 0; px < cols; ++px) {
            std::uint32_t v = y < allocH && px < allocW ? pixels[std::size_t(y) * std::size_t(allocW) + std::size_t(px)] : 0;
            if (v == 0) {
                if (transparent) {
                    continue;
                }
                v = fill;
            }
            std::uint8_t* d = &out.rgba[(std::size_t(y) * std::size_t(width) + std::size_t(px)) * 4];
            d[0] = std::uint8_t(v >> 16);
            d[1] = std::uint8_t(v >> 8);
            d[2] = std::uint8_t(v);
            d[3] = std::uint8_t(v >> 24);
        }
    }
    return true;
}

} // namespace AfterglowVt
