// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// BMP, which no common system library reads and Qt reads itself: the
// uncompressed kinds (1, 4 and 8 bits with a palette, 16 and 32 bits with
// masks, 24 bits) with the OS/2 1.x header and the Windows headers of 40 to
// 124 bytes. Run-length encoded and embedded JPEG or PNG data are refused.

#include "Formats.h"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstring>
#include <vector>

namespace AfterglowImage::detail {

namespace {

enum Compression : std::uint32_t {
    Rgb = 0,
    Bitfields = 3,
    AlphaBitfields = 6,
};

std::uint32_t le(std::string_view bytes, std::size_t index, int count)
{
    std::uint32_t value = 0;
    for (int i = count; i-- > 0;) value = value << 8 | at(bytes, index + std::size_t(i));
    return value;
}

// One channel of a pixel by its mask, scaled to 8 bits. An empty mask gives
// @p none.
class Channel
{
public:
    /// False for a mask whose bits are not next to each other.
    bool set(std::uint32_t mask)
    {
        m_mask = mask;
        if (mask == 0) return true;
        m_shift = std::countr_zero(mask);
        m_bits = std::popcount(mask);
        return (std::uint64_t(mask) >> m_shift) == (std::uint64_t(1) << m_bits) - 1;
    }
    bool empty() const { return m_mask == 0; }
    std::uint8_t operator()(std::uint32_t pixel, std::uint8_t none) const
    {
        if (m_mask == 0) return none;
        const std::uint32_t value = (pixel & m_mask) >> m_shift;
        if (m_bits >= 8) return std::uint8_t(value >> (m_bits - 8));
        return std::uint8_t(value * 255 / ((1u << m_bits) - 1));
    }

private:
    std::uint32_t m_mask = 0;
    int m_shift = 0;
    int m_bits = 0;
};

struct Header
{
    std::uint32_t size = 0; ///< of the information header
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    bool topDown = false;
    std::uint32_t bits = 0;
    std::uint32_t compression = Rgb;
    std::uint32_t colours = 0; ///< in the palette, 0 for all that the depth has
};

bool readHeader(std::string_view bytes, Header& header)
{
    if (bytes.size() < 26) return false;
    header.size = le(bytes, 14, 4);
    if (header.size == 12) {
        header.width = le(bytes, 18, 2);
        header.height = le(bytes, 20, 2);
        header.bits = le(bytes, 24, 2);
        return true;
    }
    if (header.size < 40 || header.size > 124 || bytes.size() < 14 + std::size_t(header.size)) return false;
    const auto width = std::int32_t(le(bytes, 18, 4));
    const auto height = std::int32_t(le(bytes, 22, 4));
    if (width <= 0 || height == 0 || height == INT32_MIN) return false;
    header.width = std::uint64_t(width);
    header.topDown = height < 0;
    header.height = std::uint64_t(height < 0 ? -std::int64_t(height) : height);
    header.bits = le(bytes, 28, 2);
    header.compression = le(bytes, 30, 4);
    header.colours = le(bytes, 46, 4);
    return true;
}

} // namespace

Probe probeBmp(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    Header header;
    if (!readHeader(bytes, header)) return Probe::Unknown;
    return sized(header.width, header.height, width, height);
}

bool decodeBmp(std::string_view bytes, const FrameSink& sink, std::string& error)
{
    Header header;
    int width = 0;
    int height = 0;
    if (!readHeader(bytes, header) || sized(header.width, header.height, width, height) != Probe::Image) {
        error = "the BMP has no valid header";
        return false;
    }
    const std::size_t headerEnd = 14 + std::size_t(header.size);
    const std::uint32_t bits = header.bits;
    Channel red;
    Channel green;
    Channel blue;
    Channel alpha;
    std::vector<std::array<std::uint8_t, 4>> palette;
    if (bits == 1 || bits == 4 || bits == 8) {
        if (header.compression != Rgb) {
            error = "the BMP is compressed, which is not supported";
            return false;
        }
        // Entries that the file leaves out are black.
        const std::size_t count = std::size_t(1) << bits;
        const std::size_t entry = header.size == 12 ? 3 : 4;
        const std::size_t listed = header.colours == 0 ? count : std::min<std::size_t>(header.colours, count);
        if (headerEnd + listed * entry > bytes.size()) {
            error = "the BMP is truncated";
            return false;
        }
        palette.assign(count, {0, 0, 0, 255});
        for (std::size_t i = 0; i < listed; ++i) {
            const std::size_t p = headerEnd + i * entry;
            palette[i] = {at(bytes, p + 2), at(bytes, p + 1), at(bytes, p), 255};
        }
    } else if (bits == 24 && header.compression == Rgb) {
        red.set(0xFF0000);
        green.set(0xFF00);
        blue.set(0xFF);
    } else if ((bits == 16 || bits == 32) && header.compression == Rgb) {
        // No alpha, also with a header that has an alpha mask, as Qt reads it.
        const bool wide = bits == 32;
        red.set(wide ? 0xFF0000 : 0x7C00);
        green.set(wide ? 0xFF00 : 0x3E0);
        blue.set(wide ? 0xFF : 0x1F);
    } else if ((bits == 16 || bits == 32) && (header.compression == Bitfields || header.compression == AlphaBitfields)) {
        // The masks are in a header of 52 bytes and more, and after the
        // 40-byte header otherwise. A header of 56 bytes and more, or
        // AlphaBitfields, has an alpha mask.
        const bool withAlpha = header.size >= 56 || header.compression == AlphaBitfields;
        const std::size_t masks = 14 + 40;
        if (masks + (withAlpha ? 16 : 12) > bytes.size()) {
            error = "the BMP is truncated";
            return false;
        }
        if (!red.set(le(bytes, masks, 4)) || !green.set(le(bytes, masks + 4, 4)) || !blue.set(le(bytes, masks + 8, 4))
            || !alpha.set(withAlpha ? le(bytes, masks + 12, 4) : 0)) {
            error = "the BMP has a colour mask that is not valid";
            return false;
        }
    } else {
        error = header.compression == Rgb ? "the BMP has a depth of " + std::to_string(bits) + " bits, which is not valid"
                                          : "the BMP is compressed, which is not supported";
        return false;
    }

    const std::size_t offset = le(bytes, 10, 4);
    const std::uint64_t stride = (header.width * bits + 31) / 32 * 4;
    if (offset < headerEnd || offset > bytes.size() || stride * header.height > bytes.size() - offset) {
        error = "the BMP is truncated";
        return false;
    }
    const std::size_t w = std::size_t(width);
    const std::size_t h = std::size_t(height);
    std::vector<unsigned char> rgba(w * h * 4);
    for (std::size_t y = 0; y < h; ++y) {
        // Bottom-up unless the height is negative.
        const std::size_t source = offset + std::size_t(stride) * (header.topDown ? y : h - 1 - y);
        unsigned char* out = &rgba[y * w * 4];
        for (std::size_t x = 0; x < w; ++x, out += 4) {
            if (!palette.empty()) {
                const std::size_t bit = x * bits;
                const std::uint8_t byte = at(bytes, source + bit / 8);
                const std::size_t index = (byte >> (8 - bits - bit % 8)) & ((1u << bits) - 1);
                std::memcpy(out, palette[index].data(), 4);
                continue;
            }
            const std::uint32_t pixel = le(bytes, source + x * (bits / 8), int(bits / 8));
            out[0] = red(pixel, 0);
            out[1] = green(pixel, 0);
            out[2] = blue(pixel, 0);
            out[3] = alpha(pixel, 255);
        }
    }
    sink(rgba.data(), width, height, 0);
    return true;
}

} // namespace AfterglowImage::detail
