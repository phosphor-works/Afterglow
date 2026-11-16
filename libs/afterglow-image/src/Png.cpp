// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// PNG through libpng's simplified API, which takes every colour type and
// depth and gives 8-bit RGBA with straight alpha. libpng reads no APNG: each
// frame of one goes to it as a PNG of its own, made of the file's header
// chunks and the frame's data, and this file draws the frames on the canvas
// by the rules of the PNG specification (third edition).

#include "Formats.h"

#include <png.h>
#include <zlib.h>

#include <algorithm>
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

struct Chunk
{
    std::string_view type;
    std::string_view data;
    std::string_view whole; ///< length, type, data and CRC
};

/// The chunks after the signature, up to IEND or the first one that is not
/// whole.
std::vector<Chunk> chunksOf(std::string_view bytes)
{
    std::vector<Chunk> chunks;
    for (std::size_t pos = 8; pos + 12 <= bytes.size();) {
        const std::size_t length = big(bytes, pos, 4);
        if (length > bytes.size() - pos - 12) break;
        const Chunk chunk{bytes.substr(pos + 4, 4), bytes.substr(pos + 8, length), bytes.substr(pos, 12 + length)};
        if (chunk.type == "IEND") break;
        chunks.push_back(chunk);
        pos += 12 + length;
    }
    return chunks;
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

/// One frame of an APNG: its fcTL, and its image data.
struct Frame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    int delayMs = 0;
    std::uint8_t dispose = 0; ///< 0 none, 1 background, 2 previous
    std::uint8_t blend = 0; ///< 0 source, 1 over
    std::vector<std::string_view> data; ///< IDAT data, or fdAT data without its sequence number
    bool valid = true; ///< the sequence numbers are in order
};

/// The frames of an APNG in their order, or none for a still PNG: one
/// without acTL before its image data, or whose one frame is its default
/// image.
std::vector<Frame> framesOf(const std::vector<Chunk>& chunks)
{
    std::vector<Frame> frames;
    bool animated = false;
    bool seenData = false;
    // The default image (IDAT) is the first frame only when an fcTL comes
    // before it. Else it is an image for programs that read no APNG, which
    // the animation does not show.
    bool defaultIsFrame = false;
    std::uint32_t sequence = 0;
    for (const Chunk& chunk : chunks) {
        if (chunk.type == "acTL" && !seenData && chunk.data.size() == 8) {
            animated = true;
        } else if (chunk.type == "fcTL" && animated && chunk.data.size() == 26) {
            Frame frame;
            frame.valid = big(chunk.data, 0, 4) == sequence++;
            frame.width = big(chunk.data, 4, 4);
            frame.height = big(chunk.data, 8, 4);
            frame.x = big(chunk.data, 12, 4);
            frame.y = big(chunk.data, 16, 4);
            // A denominator of 0 means 100: hundredths of a second.
            const std::uint32_t numerator = big(chunk.data, 20, 2);
            const std::uint32_t denominator = big(chunk.data, 22, 2);
            frame.delayMs = int((numerator * 1000 + (denominator ? denominator : 100) / 2) / (denominator ? denominator : 100));
            frame.dispose = at(chunk.data, 24);
            frame.blend = at(chunk.data, 25);
            frames.push_back(std::move(frame));
        } else if (chunk.type == "IDAT") {
            if (!seenData) defaultIsFrame = frames.size() == 1;
            seenData = true;
            if (defaultIsFrame && frames.size() == 1) frames.back().data.push_back(chunk.data);
        } else if (chunk.type == "fdAT" && animated && !frames.empty() && chunk.data.size() > 4) {
            if (big(chunk.data, 0, 4) != sequence++) frames.back().valid = false;
            frames.back().data.push_back(chunk.data.substr(4));
        }
    }
    if (!animated || frames.empty() || (frames.size() == 1 && defaultIsFrame)) return {};
    return frames;
}

} // namespace

Probe probePng(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    // IHDR is the first chunk. Apple's CgBI PNG has a chunk before it, and
    // holds premultiplied BGR that libpng does not read.
    if (bytes.size() < 24 || bytes.substr(12, 4) != "IHDR") return Probe::Unknown;
    return sized(big(bytes, 16, 4), big(bytes, 20, 4), width, height);
}

bool decodePng(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning)
{
    int width = 0;
    int height = 0;
    if (refused(probePng(bytes, width, height), "PNG", error)) return false;
    const std::vector<Chunk> chunks = chunksOf(bytes);
    const std::vector<Frame> frames = framesOf(chunks);
    std::vector<unsigned char> pixels;
    if (frames.empty()) {
        if (!readPng(bytes, width, height, pixels, error)) return false;
        sink(pixels.data(), width, height, 0);
        return true;
    }

    // The chunks that each frame's PNG takes from the file: those before the
    // image data that say how to read it (PLTE, tRNS, gAMA and others), not
    // the ones of the animation.
    std::string header;
    for (const Chunk& chunk : chunks) {
        if (chunk.type == "IDAT" || chunk.type == "fdAT") break;
        if (chunk.type != "IHDR" && chunk.type != "acTL" && chunk.type != "fcTL") header.append(chunk.whole);
    }
    const std::string_view ihdr = chunks.front().data;
    const std::size_t canvasBytes = std::size_t(width) * std::size_t(height) * 4;
    // The canvas starts transparent black, and each frame draws on it.
    std::vector<unsigned char> canvas(canvasBytes, 0);
    std::vector<unsigned char> saved;
    std::string png;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const Frame& frame = frames[i];
        const auto stop = [&](const std::string& why) {
            if (i == 0) {
                error = "the first frame of the APNG: " + why;
                return false;
            }
            warning = "the APNG stops at frame " + std::to_string(i + 1) + ": " + why;
            return true;
        };
        if (!frame.valid) return stop("its chunks are out of order");
        if (frame.width == 0 || frame.height == 0 || frame.x > std::uint32_t(width) || frame.width > std::uint32_t(width) - frame.x
            || frame.y > std::uint32_t(height) || frame.height > std::uint32_t(height) - frame.y) {
            return stop("it is outside the image");
        }
        if (frame.data.empty()) return stop("it has no image data");
        png.assign("\x89PNG\r\n\x1a\n", 8);
        std::string frameHeader(ihdr);
        for (int b = 0; b < 4; ++b) {
            frameHeader[std::size_t(b)] = char(frame.width >> (24 - 8 * b));
            frameHeader[std::size_t(4 + b)] = char(frame.height >> (24 - 8 * b));
        }
        appendChunk(png, "IHDR", frameHeader);
        png.append(header);
        for (const std::string_view data : frame.data) appendChunk(png, "IDAT", data);
        appendChunk(png, "IEND", {});
        std::string why;
        if (!readPng(png, int(frame.width), int(frame.height), pixels, why)) return stop(why);

        // A frame that restores the canvas after it keeps what it covers.
        const std::uint8_t dispose = i == 0 && frame.dispose == 2 ? 1 : frame.dispose;
        const auto rowAt = [&](std::vector<unsigned char>& image, std::uint32_t y) {
            return image.data() + ((std::size_t(frame.y) + y) * std::size_t(width) + frame.x) * 4;
        };
        const std::size_t rowBytes = std::size_t(frame.width) * 4;
        if (dispose == 2) {
            saved.resize(rowBytes * frame.height);
            for (std::uint32_t y = 0; y < frame.height; ++y) std::copy_n(rowAt(canvas, y), rowBytes, saved.data() + y * rowBytes);
        }
        for (std::uint32_t y = 0; y < frame.height; ++y) {
            unsigned char* to = rowAt(canvas, y);
            const unsigned char* from = pixels.data() + y * rowBytes;
            if (frame.blend == 0) {
                std::copy_n(from, rowBytes, to);
                continue;
            }
            // Over, with straight alpha.
            for (std::uint32_t x = 0; x < frame.width; ++x, from += 4, to += 4) {
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
        // An animation of one frame is a still image.
        if (!sink(canvas.data(), width, height, frames.size() == 1 ? 0 : frame.delayMs)) return true;
        if (dispose == 1) {
            for (std::uint32_t y = 0; y < frame.height; ++y) std::fill_n(rowAt(canvas, y), rowBytes, 0);
        } else if (dispose == 2) {
            for (std::uint32_t y = 0; y < frame.height; ++y) std::copy_n(saved.data() + y * rowBytes, rowBytes, rowAt(canvas, y));
        }
    }
    return true;
}

} // namespace AfterglowImage::detail
