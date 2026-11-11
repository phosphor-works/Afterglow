// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The implementation of the vendored stb_image, in its own target: it builds
// without Afterglow's warnings, as zlib-ng and xxHash do. aglow decodes from
// memory only, in a child process that Image.cpp starts with limits on its
// memory and CPU time. afterglow-mux compiles this file too, and decodes in a
// process of its own with limits (mux/src/ImageDecoder.cpp). GIF goes through aglow's own decoder below, as
// stb_image's animated GIF loader read before its heap block
// (GHSL-2023-145) and freed its delays twice (GHSL-2023-150).
//
// aglow patches tools/aglow/stb/stb_image.h in place. Each patch carries an
// "aglow patch" comment there:
//  - JPEG: a scan that uses a Huffman table that no DHT defined takes the
//    table of Annex K for tables 0 and 1, as libjpeg does (Motion JPEG
//    leaves them out), and is refused for tables 2 and 3. It decoded as grey.
//  - JPEG: an image ends at 1000 scans, as each scan walks the whole image
//    (STBI__AGLOW_MAX_JPEG_SCANS).
//  - JPEG: a marker that fails after the scans ends the image and runs
//    stbi__jpeg_finish (a progressive image without EOI kept uninitialized
//    pixels), and an image without a scan is refused.
//  - JPEG: four components without an Adobe APP14 marker are CMYK, as in
//    libjpeg, and transform 1 is YCCK (it was YCbCr with the fourth ignored).
//  - zlib: a run of fixed-code blocks builds the fixed tables once.
//  - zlib: the output of a PNG ends at the size that its image data can
//    have, so that a deflate bomb fails there (stbi__aglow_zlib_cap).
//  - PNG: the palette starts zeroed (an index past PLTE read the stack).
//  - PNG: colour types 2, 4 and 6 with a depth below 8 are refused.
//  - PNG: an IDAT longer than the rest of the input is corrupt, before the
//    buffer grows for it.
//  - BMP: a height of -2^31 is refused (abs() of it is undefined).
//  - BMP: a V3 (56-byte) header gives its masks from the header.
//  - BMP: 16 bpp BI_RGB has no alpha mask, also with a V4 or V5 header.
//  - BMP: the palette starts zeroed, an OS/2 1.x palette has the right
//    length, and a negative palette size is refused.
//  - BMP: with 16 bpp and more, the gap up to the pixels is skipped once.
//  - BMP: stbi_info gives the height of a top-down image as positive.
//  - TGA: the colour map starts at index tga_palette_start, and bit 4 of
//    the descriptor mirrors the image.
//  - PSD: 16-bit RLE channels decode byte by byte, and the matte removal
//    clamps its values.
//  - HDR: a flat scanline among run-length encoded ones decodes in place.
//  - PIC: a side longer than STBI_MAX_DIMENSIONS is "too large", as in the
//    other readers. Past 2^28 pixels, the file was not a PIC.
//  - PNM: 16-bit samples are big-endian, every sample scales from the
//    maximum value, and a maximum value of 0 is refused.
//
// The configuration: STBI_MAX_DIMENSIONS is the longest side that Afterglow
// takes, so that no size product overflows an int (convert_format16 needs
// that). Image.cpp checks the size in the header first, and reports which
// limit an image breaks. STBI_MALLOC zeroes, and STBI_REALLOC_SIZED zeroes
// what it adds, so that pixels a corrupt file never writes are black and
// transparent, never old heap contents. STBI_NO_GIF leaves stb_image's GIF
// loader out, STBI_NO_LINEAR its float API, which aglow does not use.
// STBI_FAILURE_USERMSG gives the longer messages. Every AArch64 processor
// has NEON, and SSE2 is on by itself on x86-64, so the integer IDCT of C
// runs on neither.

#include "StbImage.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

void* zeroedRealloc(void* p, std::size_t oldSize, std::size_t newSize)
{
    auto* q = static_cast<unsigned char*>(std::realloc(p, newSize));
    if (q && newSize > oldSize) {
        std::memset(q + oldSize, 0, newSize - oldSize);
    }
    return q;
}

} // namespace

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_GIF
#define STBI_NO_LINEAR
#define STBI_FAILURE_USERMSG
#define STBI_MAX_DIMENSIONS (Aglow::Stb::MaxSide)
#define STBI_MALLOC(size) std::calloc(1, size)
#define STBI_REALLOC_SIZED(p, oldSize, newSize) zeroedRealloc(p, oldSize, newSize)
#define STBI_FREE(p) std::free(p)
#if defined(__aarch64__)
#define STBI_NEON
#endif
#include <stb_image.h>

namespace Aglow::Stb {

namespace {

bool isGif(std::string_view bytes) { return bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a"); }

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

// GIF as browsers show it: each frame drawn over the canvas that the one
// before left, a graphic control extension that applies to the next frame
// only, frames clipped to the logical screen, and LZW streams with no
// leading clear code or with a clear deferred past a full table. One rule is
// stb_image's and not the browsers': the pixels that the first frame does
// not cover show the background colour of the global table, where a browser
// leaves them transparent.
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
    // Draws the image data of one frame. False when it is corrupt; data that
    // ends early leaves the rest of the frame as it was, as in browsers.
    bool raster(const Palette& palette, const Rect& rect, bool interlaced);

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

bool GifDecoder::raster(const Palette& palette, const Rect& rect, bool interlaced)
{
    const int minCode = m_in.byte();
    if (m_in.truncated()) {
        return true;
    }
    // GIF89a takes 2 as the least code size, also for an image of two colours.
    if (minCode < 2 || minCode > 8) {
        return false;
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
    return !corrupt;
}

bool GifDecoder::run(const FrameSink& sink, std::string& error, std::string& warning)
{
    m_in.skip(6);
    m_width = m_in.u16();
    m_height = m_in.u16();
    const auto flags = m_in.byte();
    const std::size_t background = m_in.byte();
    m_in.byte(); // the pixel aspect ratio
    if (m_in.truncated()) {
        error = "the GIF is truncated";
        return false;
    }
    if (m_width == 0 || m_height == 0) {
        error = "the GIF has no pixels";
        return false;
    }
    if (m_width > std::size_t(MaxSide) || m_height > std::size_t(MaxSide)) {
        error = "the GIF has a side longer than " + std::to_string(MaxSide) + " pixels";
        return false;
    }
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
    for (;;) {
        if (m_in.atEnd()) {
            // A GIF without its trailer: the frames so far.
            if (frames == 0) {
                error = "the GIF has no image";
                return false;
            }
            return true;
        }
        const auto kind = m_in.byte();
        if (kind == 0x3B) {
            return true;
        }
        if (kind == 0x21) {
            const auto label = m_in.byte();
            if (label == 0xF9) {
                const std::size_t length = m_in.byte();
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
        Palette palette {};
        if (local & 0x80) {
            readTable(palette, std::size_t(2) << (local & 7));
        } else if (globalCount > 0) {
            palette = global;
        } else {
            return damaged("a frame of the GIF has no colour table");
        }
        if (m_in.truncated()) {
            // Cut off in its descriptor: no frame at all.
            if (frames == 0) {
                error = "the GIF is truncated";
                return false;
            }
            return true;
        }
        if (transparent >= 0) {
            palette[std::size_t(transparent)][3] = 0;
        }
        if (frames == 0 && background > 0 && background < globalCount && int(background) != transparent) {
            // stb_image's rule, in RGB order: the pixels that the first frame
            // does not cover show the background colour of the global table.
            std::array<std::uint8_t, 4> colour = global[background];
            colour[3] = 255;
            fillRect({ 0, 0, m_width, m_height }, colour);
            fillRect(rect, { 0, 0, 0, 0 });
        }
        if (disposal == 3) {
            saved = m_canvas;
        }
        if (!raster(palette, rect, (local & 0x40) != 0)) {
            return damaged("the image data of the GIF is corrupt");
        }
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

Probe probe(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    if (isGif(bytes)) {
        if (bytes.size() < 10) {
            return Probe::Unknown;
        }
        width = int(std::uint8_t(bytes[6]) | std::uint8_t(bytes[7]) << 8);
        height = int(std::uint8_t(bytes[8]) | std::uint8_t(bytes[9]) << 8);
    } else {
        if (bytes.size() > std::size_t(INT_MAX)) {
            return Probe::Unknown;
        }
        // The formats in the order of stbi__info_main, one at a time: a side
        // past STBI_MAX_DIMENSIONS is refused by its own reader, and a later
        // reader would replace that reason. The reader has stored the side.
        using Info = int (*)(stbi__context*, int*, int*, int*);
        static constexpr Info readers[] = { stbi__jpeg_info, stbi__png_info, stbi__bmp_info, stbi__psd_info,
            stbi__pic_info, stbi__pnm_info, stbi__hdr_info, stbi__tga_info };
        stbi__context context;
        stbi__start_mem(&context, reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()));
        bool known = false;
        for (const Info reader : readers) {
            int channels = 0;
            stbi__g_failure_reason = nullptr;
            // A reader gives its bit depth (8 or 16 for PNM), not only 1.
            if (reader(&context, &width, &height, &channels) != 0) {
                known = true;
                break;
            }
            const char* reason = stbi__g_failure_reason;
            if (reason && std::strcmp(reason, "Very large image (corrupt?)") == 0) {
                width = int(std::min<stbi__uint32>(context.img_x, INT_MAX));
                height = int(std::min<stbi__uint32>(context.img_y, INT_MAX));
                return Probe::TooLarge;
            }
            stbi__rewind(&context);
        }
        if (!known) {
            return Probe::Unknown;
        }
    }
    if (width > MaxSide || height > MaxSide) {
        return Probe::TooLarge;
    }
    return width > 0 && height > 0 ? Probe::Image : Probe::Unknown;
}

bool decode(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning)
{
    if (isGif(bytes)) {
        return GifDecoder(bytes).run(sink, error, warning);
    }
    if (bytes.size() > std::size_t(INT_MAX)) {
        error = "the file is too large";
        return false;
    }
    // Apple's CgBI PNG holds premultiplied BGR: straight RGB, as every other PNG.
    stbi_convert_iphone_png_to_rgb(1);
    stbi_set_unpremultiply_on_load(1);
    stbi__g_failure_reason = nullptr;
    int width = 0;
    int height = 0;
    int channels = 0;
    std::unique_ptr<stbi_uc, void (*)(void*)> pixels(
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &width, &height, &channels, 4),
        stbi_image_free);
    if (!pixels) {
        const char* reason = stbi_failure_reason();
        error = reason && *reason ? reason : "the image data is corrupt";
        return false;
    }
    sink(pixels.get(), width, height, 0);
    return true;
}

} // namespace Aglow::Stb
