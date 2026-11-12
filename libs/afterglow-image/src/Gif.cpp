// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The library's own GIF decoder: it composes the frames of an animation, and
// its LZW reader bounds its work by the canvas and the data, whatever size a
// frame claims. tools/fuzz/imagefuzz.cpp fuzzes it.

#include "Formats.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace AfterglowImage::detail {

namespace {

// Bytes in order, and 0 past the end.
class Reader
{
public:
    explicit Reader(std::string_view bytes)
        : m_bytes(bytes)
    {
    }
    bool atEnd() const { return m_at >= m_bytes.size(); }
    bool truncated() const { return m_truncated; }
    std::uint8_t byte()
    {
        if (atEnd()) {
            m_truncated = true;
            return 0;
        }
        return std::uint8_t(m_bytes[m_at++]);
    }
    std::size_t u16()
    {
        const std::size_t low = byte();
        return low | std::size_t(byte()) << 8;
    }
    void skip(std::size_t n)
    {
        if (n > m_bytes.size() - m_at) {
            m_at = m_bytes.size();
            m_truncated = true;
            return;
        }
        m_at += n;
    }
    // Data sub-blocks, up to the empty one that ends them.
    void skipBlocks()
    {
        while (!atEnd()) {
            const auto length = byte();
            if (length == 0) {
                return;
            }
            skip(length);
        }
    }

private:
    std::string_view m_bytes;
    std::size_t m_at = 0;
    bool m_truncated = false;
};

// GIF as browsers and kitty show it: each frame drawn over the canvas that
// the one before left, a graphic control extension that applies to the next
// frame only, LZW streams with no leading clear code or with a clear deferred
// past a full table, and a canvas that starts transparent: the background
// colour of the global table is not used. Frames are clipped to the logical
// screen, and a logical screen with an empty side is refused.
class GifDecoder
{
public:
    explicit GifDecoder(std::string_view bytes)
        : m_in(bytes)
    {
    }

    bool run(const FrameSink& sink, std::string& error, std::string& warning);

private:
    using Palette = std::array<std::array<std::uint8_t, 4>, 256>;
    static constexpr std::size_t MaxCodes = 4096;

    struct Rect
    {
        std::size_t left = 0;
        std::size_t top = 0;
        std::size_t width = 0;
        std::size_t height = 0;
    };

    void readTable(Palette& table, std::size_t count);
    // Fills the part of the frame rectangle that is on the canvas.
    void fillRect(const Rect& rect, const std::array<std::uint8_t, 4>& colour);
    // Draws the image data of one frame. Data that ends early leaves the
    // rest of the frame as it was, as in browsers. Data that ends before its
    // first pixel draws no frame: Cut.
    enum class Raster : std::uint8_t { Drawn, Corrupt, Cut };
    Raster raster(const Palette& palette, const Rect& rect, bool interlaced);

    Reader m_in;
    std::size_t m_width = 0;
    std::size_t m_height = 0;
    std::vector<std::uint8_t> m_canvas;
    std::vector<std::uint16_t> m_prefix = std::vector<std::uint16_t>(MaxCodes);
    std::vector<std::uint16_t> m_length = std::vector<std::uint16_t>(MaxCodes);
    std::vector<std::uint8_t> m_suffix = std::vector<std::uint8_t>(MaxCodes);
    std::vector<std::uint8_t> m_first = std::vector<std::uint8_t>(MaxCodes);
    std::vector<std::uint8_t> m_string = std::vector<std::uint8_t>(MaxCodes);
};

void GifDecoder::readTable(Palette& table, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        const auto red = m_in.byte();
        const auto green = m_in.byte();
        const auto blue = m_in.byte();
        table[i] = { red, green, blue, 255 };
    }
}

void GifDecoder::fillRect(const Rect& rect, const std::array<std::uint8_t, 4>& colour)
{
    const std::size_t right = std::min(m_width, rect.left + rect.width);
    const std::size_t bottom = std::min(m_height, rect.top + rect.height);
    for (std::size_t y = rect.top; y < bottom; ++y) {
        for (std::size_t x = rect.left; x < right; ++x) {
            std::memcpy(&m_canvas[(y * m_width + x) * 4], colour.data(), 4);
        }
    }
}

GifDecoder::Raster GifDecoder::raster(const Palette& palette, const Rect& rect, bool interlaced)
{
    const int minCode = m_in.byte();
    if (m_in.truncated()) {
        return Raster::Cut;
    }
    // GIF89a takes 2 as the least code size, also for an image of two colours.
    if (minCode < 2 || minCode > 8) {
        return Raster::Corrupt;
    }
    // The sub-blocks, as one stream of bytes.
    std::size_t blockLeft = 0;
    bool ended = false;
    bool terminated = false;
    auto next = [&]() -> int {
        if (ended) {
            return -1;
        }
        if (blockLeft == 0) {
            blockLeft = m_in.byte();
            if (blockLeft == 0) {
                ended = true;
                terminated = !m_in.truncated();
                return -1;
            }
        }
        --blockLeft;
        const int value = m_in.byte();
        if (m_in.truncated()) {
            ended = true;
            return -1;
        }
        return value;
    };

    const std::size_t total = rect.width * rect.height;
    // The part of the frame that is on the canvas: its first visibleWidth
    // columns of its first visibleHeight rows.
    const std::size_t visibleWidth = rect.left < m_width ? std::min(rect.width, m_width - rect.left) : 0;
    const std::size_t visibleHeight = rect.top < m_height ? std::min(rect.height, m_height - rect.top) : 0;
    const std::size_t visible = visibleWidth * visibleHeight;
    std::size_t done = 0;
    std::size_t landed = 0; ///< pixels of the frame that were on the canvas
    std::size_t column = 0;
    std::size_t row = 0;
    int pass = 0;
    static constexpr std::size_t PassStart[4] = { 0, 4, 2, 1 };
    static constexpr std::size_t PassStep[4] = { 8, 8, 4, 2 };
    std::size_t step = interlaced ? PassStep[0] : 1;
    // Moves @p n pixels on, up to the end of the row or of the pass at most.
    auto advance = [&](std::size_t n) {
        done += n;
        column += n;
        if (column < rect.width) {
            return;
        }
        row += column / rect.width * step;
        column %= rect.width;
        while (interlaced && row >= rect.height && pass < 3) {
            ++pass;
            row = PassStart[pass];
            step = PassStep[pass];
        }
    };
    // A frame can claim 65535 pixels a side on a canvas of one. Pixels off
    // the canvas move on as one run, up to the end of their row or, below
    // the canvas, of their pass, and decoding stops when no pixel that is
    // left can land on the canvas. A string is spelled out only when one of
    // its pixels lands, so the work is bounded by the canvas and the data,
    // whatever size the frame claims.
    auto emit = [&](std::size_t code) {
        const std::size_t length = m_length[code];
        bool spelled = false;
        for (std::size_t at = 0; at < length && done < total;) {
            std::size_t run = 0;
            if (row < visibleHeight && column < visibleWidth) {
                if (!spelled) {
                    for (std::size_t i = length, c = code; i-- > 0;) {
                        m_string[i] = m_suffix[c];
                        c = m_prefix[c];
                    }
                    spelled = true;
                }
                run = std::min(length - at, visibleWidth - column);
                std::uint8_t* out = &m_canvas[((rect.top + row) * m_width + rect.left + column) * 4];
                for (std::size_t i = 0; i < run; ++i) {
                    const auto& colour = palette[m_string[at + i]];
                    if (colour[3] != 0) {
                        std::memcpy(out + i * 4, colour.data(), 4);
                    }
                }
                landed += run;
            } else if (row < visibleHeight) {
                run = std::min(length - at, rect.width - column);
            } else {
                const std::size_t rows = (rect.height - 1 - row) / step + 1;
                run = std::min(length - at, rows * rect.width - column);
            }
            run = std::min(run, total - done);
            at += run;
            advance(run);
        }
    };

    const std::size_t clear = std::size_t(1) << minCode;
    const std::size_t end = clear + 1;
    for (std::size_t c = 0; c < clear; ++c) {
        m_prefix[c] = 0;
        m_suffix[c] = std::uint8_t(c);
        m_first[c] = std::uint8_t(c);
        m_length[c] = 1;
    }
    int size = minCode + 1;
    std::size_t available = clear + 2;
    std::ptrdiff_t previous = -1;
    std::uint32_t bits = 0;
    int count = 0;
    bool corrupt = false;
    while (done < total && landed < visible) {
        bool more = true;
        while (count < size) {
            const int value = next();
            if (value < 0) {
                more = false;
                break;
            }
            bits |= std::uint32_t(value) << count;
            count += 8;
        }
        if (!more) {
            break;
        }
        const std::size_t code = bits & ((std::uint32_t(1) << size) - 1);
        bits >>= size;
        count -= size;
        if (code == clear) {
            size = minCode + 1;
            available = clear + 2;
            previous = -1;
            continue;
        }
        if (code == end) {
            break;
        }
        if (previous < 0) {
            if (code >= clear) {
                corrupt = true;
                break;
            }
            emit(code);
            previous = std::ptrdiff_t(code);
            continue;
        }
        if (code > available) {
            corrupt = true;
            break;
        }
        // A full table takes no more codes until the next clear.
        if (available < MaxCodes) {
            const auto before = std::size_t(previous);
            m_prefix[available] = std::uint16_t(before);
            m_first[available] = m_first[before];
            m_suffix[available] = code < available ? m_first[code] : m_first[before];
            m_length[available] = std::uint16_t(m_length[before] + 1);
            ++available;
            if (available == (std::size_t(1) << size) && size < 12) {
                ++size;
            }
        }
        emit(code);
        previous = std::ptrdiff_t(code);
    }
    // What is left of the data, up to the terminator.
    if (!terminated && !m_in.truncated()) {
        m_in.skip(blockLeft);
        m_in.skipBlocks();
    }
    if (corrupt) return Raster::Corrupt;
    return done == 0 && m_in.truncated() ? Raster::Cut : Raster::Drawn;
}

bool GifDecoder::run(const FrameSink& sink, std::string& error, std::string& warning)
{
    m_in.skip(6);
    m_width = m_in.u16();
    m_height = m_in.u16();
    const auto flags = m_in.byte();
    m_in.byte(); // the index of the background colour
    m_in.byte(); // the pixel aspect ratio
    if (m_in.truncated()) {
        error = "the GIF is truncated";
        return false;
    }
    int width = 0;
    int height = 0;
    if (refused(sized(m_width, m_height, width, height), "GIF", error)) return false;
    Palette global {};
    std::size_t globalCount = 0;
    if (flags & 0x80) {
        globalCount = std::size_t(2) << (flags & 7);
        readTable(global, globalCount);
    }
    m_canvas.assign(m_width * m_height * 4, 0);
    std::vector<std::uint8_t> saved;
    // From the graphic control extension, for the next frame only.
    int disposal = 0;
    int delay = 0;
    int transparent = -1;
    int frames = 0;
    auto damaged = [&](const std::string& what) {
        if (frames == 0) {
            error = what;
            return false;
        }
        warning = what + ", so the animation stops after frame " + std::to_string(frames);
        return true;
    };
    // The end of the blocks: the frames so far, and no image is an error.
    // A file that ends inside a block keeps its frames, the last of which
    // can lack rows, and the warning says so.
    auto ended = [&] {
        if (frames == 0) {
            error = m_in.truncated() ? "the GIF is truncated" : "the GIF has no image";
            return false;
        }
        if (m_in.truncated()) warning = "the GIF is cut short, so it ends in frame " + std::to_string(frames);
        return true;
    };
    for (;;) {
        // A GIF without its trailer ends like one with it.
        if (m_in.atEnd()) return ended();
        const auto kind = m_in.byte();
        if (kind == 0x3B) return ended();
        if (kind == 0x21) {
            const auto label = m_in.byte();
            if (label == 0xF9) {
                const std::size_t length = m_in.byte();
                // An empty first block is the terminator of the extension.
                if (length == 0) continue;
                if (length >= 4) {
                    const auto packed = m_in.byte();
                    delay = int(m_in.u16()) * 10;
                    const auto index = m_in.byte();
                    m_in.skip(length - 4);
                    disposal = (packed >> 2) & 7;
                    transparent = (packed & 1) ? int(index) : -1;
                } else {
                    m_in.skip(length);
                }
            }
            m_in.skipBlocks();
            continue;
        }
        if (kind != 0x2C) {
            return damaged("the GIF has a block of an unknown kind");
        }
        Rect rect;
        rect.left = m_in.u16();
        rect.top = m_in.u16();
        rect.width = m_in.u16();
        rect.height = m_in.u16();
        const auto local = m_in.byte();
        // Cut off in its descriptor: no frame from it.
        if (m_in.truncated()) return ended();
        Palette palette {};
        if (local & 0x80) {
            readTable(palette, std::size_t(2) << (local & 7));
            if (m_in.truncated()) return ended();
        } else if (globalCount > 0) {
            palette = global;
        } else {
            return damaged("a frame of the GIF has no color table");
        }
        if (transparent >= 0) {
            palette[std::size_t(transparent)][3] = 0;
        }
        if (disposal == 3) {
            saved = m_canvas;
        }
        const Raster drawn = raster(palette, rect, (local & 0x40) != 0);
        if (drawn == Raster::Corrupt) {
            return damaged("the image data of the GIF is corrupt");
        }
        // Cut off before its first pixel: no frame from it either.
        if (drawn == Raster::Cut) return ended();
        ++frames;
        if (!sink(m_canvas.data(), int(m_width), int(m_height), delay)) {
            return true;
        }
        // The frame's disposal, before the next frame is drawn.
        if (disposal == 2) {
            fillRect(rect, { 0, 0, 0, 0 });
        } else if (disposal == 3) {
            m_canvas.swap(saved);
        }
        disposal = 0;
        delay = 0;
        transparent = -1;
    }
}

} // namespace

Probe probeGif(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    if (bytes.size() < 10) return Probe::Unknown;
    return sized(std::uint64_t(at(bytes, 6) | at(bytes, 7) << 8), std::uint64_t(at(bytes, 8) | at(bytes, 9) << 8), width, height);
}

bool decodeGif(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning)
{
    return GifDecoder(bytes).run(sink, error, warning);
}

} // namespace AfterglowImage::detail
