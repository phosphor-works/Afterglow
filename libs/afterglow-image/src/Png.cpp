// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// PNG through libpng's simplified API, which takes every colour type and
// depth and gives 8-bit RGBA with straight alpha. libpng reads no APNG: each
// frame of one goes to it as a PNG of its own, made of the chunks of the file
// that libpng reads for its pixels and the frame's data, and this file draws
// the frames on the canvas by the rules of the PNG specification (third
// edition), one at a time.

#include "Formats.h"

#include <png.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace AfterglowImage::detail {

namespace {

/// Decodes the PNG @p bytes, whose IHDR says @p width by @p height.
bool readPng(std::string_view bytes, int width, int height, std::vector<unsigned char>& pixels, std::string& error)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    // Frees what libpng holds, also after a failure, which frees it itself.
    const std::unique_ptr<png_image, void (*)(png_imagep)> guard(&image, png_image_free);
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) {
        error = libraryMessage(image.message);
        return false;
    }
    // The header that libpng read is the one that the caller checked.
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
    pixels.resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) {
        error = libraryMessage(image.message);
        return false;
    }
    return true;
}

/// An iCCP chunk's profile: its name, a NUL, the compression method (0) and
/// the profile deflated with zlib.
std::string inflateIcc(std::string_view chunk)
{
    const std::size_t nul = chunk.find('\0');
    if (nul == std::string_view::npos || nul == 0 || nul > 79 || nul + 2 > chunk.size() || chunk[nul + 1] != 0) return {};
    const std::string_view deflated = chunk.substr(nul + 2);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) return {};
    std::string out;
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(deflated.data()));
    stream.avail_in = uInt(deflated.size());
    std::array<char, 16384> buffer{};
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = uInt(buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        out.append(buffer.data(), buffer.size() - stream.avail_out);
        if (out.size() > MaxIcc) result = Z_MEM_ERROR;
    }
    inflateEnd(&stream);
    return result == Z_STREAM_END ? out : std::string();
}

void appendChunk(std::string& out, std::string_view type, std::string_view data)
{
    const auto put32 = [&](std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(char(value >> shift));
    };
    put32(std::uint32_t(data.size()));
    out.append(type);
    out.append(data);
    uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type.data()), uInt(type.size()));
    crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data()), uInt(data.size()));
    put32(std::uint32_t(crc));
}

/// The chunks that say how the samples relate to light. libpng's
/// simplified API applies gAMA, and sRGB as its curve.
bool isColorChunk(std::string_view type)
{
    return type == "gAMA" || type == "cHRM" || type == "sRGB" || type == "iCCP" || type == "cICP";
}

/// A chunk before the image data that libpng's simplified API reads for
/// RGBA: the palette, its transparency and the colour chunks (not iCCP,
/// which it does not read), unless the converter applies the colour space.
bool readsChunk(std::string_view type, bool colorKnown)
{
    return type == "PLTE" || type == "tRNS" || (!colorKnown && type != "iCCP" && isColorChunk(type));
}

constexpr std::string_view Signature("\x89PNG\r\n\x1a\n", 8);

/// Decodes the default image (IDAT) of the PNG @p bytes, also of an APNG,
/// whose IHDR says @p width by @p height. With @p colorKnown, it goes to
/// libpng without its colour chunks.
bool decodeStill(std::string_view bytes, int width, int height, bool colorKnown, const FrameSink& sink, std::string& error)
{
    std::string rebuilt;
    if (colorKnown) {
        // The chunks that libpng reads, as they are. Only the image data is
        // large: in a real file, about the size of its pixels, which the
        // memory limit of a decoding process allows several times over.
        rebuilt.assign(Signature);
        forEachPngChunk(bytes, [&](std::string_view type, std::string_view, std::string_view whole) {
            if (type == "IHDR" || type == "IDAT" || readsChunk(type, true)) rebuilt.append(whole);
            return true;
        });
        appendChunk(rebuilt, "IEND", {});
        bytes = rebuilt;
    }
    std::vector<unsigned char> pixels;
    if (!readPng(bytes, width, height, pixels, error)) return false;
    sink(pixels.data(), width, height, 0);
    return true;
}

/// What the chunks of a PNG say of its animation (the PNG specification,
/// third edition).
struct Animation
{
    /// The fcTL chunks after an acTL that comes before the image data. 0
    /// for a still PNG, also for an APNG whose one frame is its default
    /// image.
    std::size_t frames = 0;
    /// An fcTL comes before the default image (IDAT), which is then the
    /// first frame. Else it is an image for programs that read no APNG,
    /// which the animation does not show.
    bool defaultIsFrame = false;
    /// The chunks before the image data that each frame's PNG takes:
    /// readsChunk(), the first of each type, no larger than the largest
    /// that is valid (a PLTE of 256 colours). Any other would cost its size
    /// again in each frame.
    std::string header;
};

Animation animationOf(std::string_view bytes, bool colorKnown)
{
    Animation animation;
    bool animated = false;
    bool seenIdat = false;
    bool seenData = false;
    std::vector<std::string_view> types;
    // ApngReader::run() takes the same chunks as the frames.
    forEachPngChunk(bytes, [&](std::string_view type, std::string_view data, std::string_view whole) {
        if (type == "acTL" && !seenIdat && data.size() == 8) {
            animated = true;
        } else if (type == "fcTL" && animated && data.size() == 26) {
            ++animation.frames;
        } else if (type == "IDAT") {
            if (!seenIdat) animation.defaultIsFrame = animation.frames == 1;
            seenIdat = seenData = true;
        } else if (type == "fdAT") {
            seenData = true;
        } else if (!seenData && readsChunk(type, colorKnown) && data.size() <= 768
                   && std::find(types.begin(), types.end(), type) == types.end()) {
            types.push_back(type);
            animation.header.append(whole);
        }
        return true;
    });
    if (!animated || (animation.frames == 1 && animation.defaultIsFrame)) animation.frames = 0;
    return animation;
}

/// Draws the frames of an APNG on its canvas by the rules of the PNG
/// specification, one at a time: each frame's PNG holds only its own data.
class ApngReader
{
public:
    ApngReader(std::string_view bytes, int width, int height, const Animation& animation, const FrameSink& sink)
        : m_bytes(bytes)
        , m_width(width)
        , m_height(height)
        , m_animation(animation)
        , m_sink(sink)
        , m_canvas(std::size_t(width) * std::size_t(height) * 4, 0) // transparent black
    {
    }

    /// False, with @p why, when the first frame is not valid: then no
    /// frame went to the sink. Else true, with @p why when a later frame
    /// is not valid (stoppedAt()).
    bool run(std::string& why)
    {
        bool animated = false;
        bool seenIdat = false;
        bool done = false;
        forEachPngChunk(m_bytes, [&](std::string_view type, std::string_view data, std::string_view) {
            if (type == "acTL" && !seenIdat && data.size() == 8) {
                animated = true;
            } else if (type == "fcTL" && animated && data.size() == 26) {
                if (m_index >= 0 && !finish(why)) done = true;
                if (!done) start(data);
            } else if (type == "IDAT") {
                // The default image is the data of the first frame.
                if (m_animation.defaultIsFrame && m_index == 0) add(data);
                seenIdat = true;
            } else if (type == "fdAT" && animated && m_index >= 0) {
                // A sequence number with no data still counts.
                if (data.size() < 4 || big(data, 0, 4) != m_sequence++) m_inOrder = false;
                if (data.size() > 4) add(data.substr(4));
            }
            return !done;
        });
        if (!done && m_index >= 0) finish(why);
        if (m_shown > 0) return true;
        if (why.empty()) why = "it is not valid";
        return false;
    }

    /// The number of the frame that run() stopped at, from 1.
    int stoppedAt() const { return m_index + 1; }

private:
    void start(std::string_view fcTL)
    {
        ++m_index;
        m_inOrder = big(fcTL, 0, 4) == m_sequence++;
        m_frameWidth = big(fcTL, 4, 4);
        m_frameHeight = big(fcTL, 8, 4);
        m_x = big(fcTL, 12, 4);
        m_y = big(fcTL, 16, 4);
        // A denominator of 0 means 100: hundredths of a second.
        const std::uint32_t numerator = big(fcTL, 20, 2);
        const std::uint32_t denominator = big(fcTL, 22, 2) ? big(fcTL, 22, 2) : 100;
        m_delayMs = int((numerator * 1000 + denominator / 2) / denominator);
        m_dispose = at(fcTL, 24);
        m_blend = at(fcTL, 25);
        m_inside = m_frameWidth != 0 && m_frameHeight != 0 && m_x <= std::uint32_t(m_width) && m_frameWidth <= std::uint32_t(m_width) - m_x
            && m_y <= std::uint32_t(m_height) && m_frameHeight <= std::uint32_t(m_height) - m_y;
        m_hasData = false;
        m_png.clear();
        if (!m_inside) return;
        // The file's IHDR with the frame's size, which the probe checked
        // is 13 bytes long.
        std::string ihdr(m_bytes.substr(16, 13));
        for (int b = 0; b < 4; ++b) {
            ihdr[std::size_t(b)] = char(m_frameWidth >> (24 - 8 * b));
            ihdr[std::size_t(4 + b)] = char(m_frameHeight >> (24 - 8 * b));
        }
        m_png.assign(Signature);
        appendChunk(m_png, "IHDR", ihdr);
        m_png.append(m_animation.header);
    }

    void add(std::string_view data)
    {
        m_hasData = true;
        if (m_inside) appendChunk(m_png, "IDAT", data);
    }

    /// Draws the frame, and gives it to the sink. False when the
    /// animation ends here: a frame that is not valid (with @p why), or a
    /// sink that says stop.
    bool finish(std::string& why)
    {
        if (!m_inOrder) {
            why = "its chunks are out of order";
            return false;
        }
        if (!m_inside) {
            why = "it is outside the image";
            return false;
        }
        if (!m_hasData) {
            why = "it has no image data";
            return false;
        }
        appendChunk(m_png, "IEND", {});
        if (!readPng(m_png, int(m_frameWidth), int(m_frameHeight), m_pixels, why)) return false;

        // A first frame that restores the canvas after it keeps what it covers.
        const std::uint8_t dispose = m_index == 0 && m_dispose == 2 ? 1 : m_dispose;
        const auto rowAt = [&](std::vector<unsigned char>& image, std::uint32_t y) {
            return image.data() + ((std::size_t(m_y) + y) * std::size_t(m_width) + m_x) * 4;
        };
        const std::size_t rowBytes = std::size_t(m_frameWidth) * 4;
        if (dispose == 2) {
            m_saved.resize(rowBytes * m_frameHeight);
            for (std::uint32_t y = 0; y < m_frameHeight; ++y) std::copy_n(rowAt(m_canvas, y), rowBytes, m_saved.data() + y * rowBytes);
        }
        for (std::uint32_t y = 0; y < m_frameHeight; ++y) {
            unsigned char* to = rowAt(m_canvas, y);
            const unsigned char* from = m_pixels.data() + y * rowBytes;
            if (m_blend == 0) {
                std::copy_n(from, rowBytes, to);
                continue;
            }
            // Over, with straight alpha.
            for (std::uint32_t x = 0; x < m_frameWidth; ++x, from += 4, to += 4) {
                const std::uint32_t a = from[3];
                if (a == 255 || to[3] == 0) {
                    std::copy_n(from, 4, to);
                } else if (a != 0) {
                    const std::uint32_t under = to[3] * (255 - a);
                    const std::uint32_t out = a * 255 + under;
                    for (int c = 0; c < 3; ++c) to[c] = std::uint8_t((from[c] * a * 255 + to[c] * under + out / 2) / out);
                    to[3] = std::uint8_t((out + 127) / 255);
                }
            }
        }
        ++m_shown;
        // An animation of one frame is a still image.
        if (!m_sink(m_canvas.data(), m_width, m_height, m_animation.frames == 1 ? 0 : m_delayMs)) return false;
        if (dispose == 1) {
            for (std::uint32_t y = 0; y < m_frameHeight; ++y) std::fill_n(rowAt(m_canvas, y), rowBytes, 0);
        } else if (dispose == 2) {
            for (std::uint32_t y = 0; y < m_frameHeight; ++y) std::copy_n(m_saved.data() + y * rowBytes, rowBytes, rowAt(m_canvas, y));
        }
        return true;
    }

    std::string_view m_bytes;
    int m_width;
    int m_height;
    const Animation& m_animation;
    const FrameSink& m_sink;
    std::vector<unsigned char> m_canvas;
    std::vector<unsigned char> m_saved;
    std::vector<unsigned char> m_pixels;
    std::string m_png; ///< the frame's PNG, made as its chunks come
    std::uint32_t m_sequence = 0;
    int m_index = -1; ///< of the frame that its chunks make
    std::size_t m_shown = 0;
    // The frame's fcTL.
    std::uint32_t m_frameWidth = 0;
    std::uint32_t m_frameHeight = 0;
    std::uint32_t m_x = 0;
    std::uint32_t m_y = 0;
    int m_delayMs = 0;
    std::uint8_t m_dispose = 0; ///< 0 none, 1 background, 2 previous
    std::uint8_t m_blend = 0; ///< 0 source, 1 over
    bool m_inOrder = true; ///< the sequence numbers of its chunks
    bool m_inside = true;
    bool m_hasData = false;
};

} // namespace

Probe probePng(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    // IHDR is the first chunk, of 13 bytes. Apple's CgBI PNG has a chunk
    // before it, and holds premultiplied BGR that libpng does not read.
    if (bytes.size() < 29 || big(bytes, 8, 4) != 13 || bytes.substr(12, 4) != "IHDR") return Probe::Unknown;
    return sized(big(bytes, 16, 4), big(bytes, 20, 4), width, height);
}

ColorSpace pngColorSpace(std::string_view bytes)
{
    ColorSpace space;
    // The colour types 0 and 4 are grey.
    space.grey = (at(bytes, 25) & 2) == 0;
    // The colour chunks come before the image data.
    forEachPngChunk(bytes, [&](std::string_view type, std::string_view data, std::string_view) {
        if (type == "IDAT") return false;
        if (type == "cICP" && data.size() == 4) {
            space.cicp = true;
            space.primaries = at(data, 0);
            space.transfer = at(data, 1);
            space.matrix = at(data, 2);
            space.fullRange = at(data, 3) == 1;
        } else if (type == "iCCP" && space.icc.empty()) {
            space.icc = inflateIcc(data);
        }
        return true;
    });
    return space;
}

bool decodePng(std::string_view bytes, const FrameSink& sink, const Options& options, std::string& error, std::string& warning)
{
    int width = 0;
    int height = 0;
    if (refused(probePng(bytes, width, height), "PNG", error)) return false;
    if (options.pngFrames == PngFrames::DefaultImage) return decodeStill(bytes, width, height, options.colorKnown, sink, error);
    const Animation animation = animationOf(bytes, options.colorKnown);
    if (animation.frames == 0) return decodeStill(bytes, width, height, options.colorKnown, sink, error);
    ApngReader reader(bytes, width, height, animation, sink);
    std::string why;
    if (reader.run(why)) {
        if (!why.empty()) warning = "the APNG stops at frame " + std::to_string(reader.stoppedAt()) + ": " + why;
        return true;
    }
    // An error before the animation starts shows the default image, as the
    // APNG specification asks.
    if (!decodeStill(bytes, width, height, options.colorKnown, sink, error)) return false;
    warning = "the APNG shows its default image, as its first frame is not valid: " + why;
    return true;
}

} // namespace AfterglowImage::detail
