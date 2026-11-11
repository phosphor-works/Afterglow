// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The decoders in this process: the formats, their sizes from the header,
// and the files that they refuse. aglow's icat_decode.py and the tests of
// afterglow-mux run them in their child processes.

#include "ImageFiles.h"

#include <AfterglowImage/Decode.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace AfterglowImage;

namespace {

struct Frame
{
    int width = 0;
    int height = 0;
    int delay = 0;
    std::vector<std::uint8_t> rgba;
};

struct Result
{
    bool ok = false;
    std::vector<Frame> frames;
    std::string error;
    std::string warning;
};

Result run(std::string_view bytes, std::size_t maxFrames = 100)
{
    Result r;
    r.ok = decode(bytes, [&](const unsigned char* rgba, int width, int height, int delay) {
        r.frames.push_back({width, height, delay, std::vector<std::uint8_t>(rgba, rgba + std::size_t(width) * std::size_t(height) * 4)});
        return r.frames.size() < maxFrames;
    }, r.error, r.warning);
    return r;
}

std::vector<std::uint8_t> repeat(std::vector<std::uint8_t> pixel, int count)
{
    std::vector<std::uint8_t> out;
    for (int i = 0; i < count; ++i) out.insert(out.end(), pixel.begin(), pixel.end());
    return out;
}

// Each pixel near @p r, @p g, @p b and opaque: a lossy format.
bool near(const Frame& f, int r, int g, int b)
{
    for (std::size_t i = 0; i < f.rgba.size(); i += 4) {
        if (std::abs(f.rgba[i] - r) > 16 || std::abs(f.rgba[i + 1] - g) > 16 || std::abs(f.rgba[i + 2] - b) > 16 || f.rgba[i + 3] != 255)
            return false;
    }
    return !f.rgba.empty();
}

std::string le(std::uint32_t value, int bytes)
{
    std::string out;
    for (int i = 0; i < bytes; ++i) out += char(value >> (8 * i) & 0xFF);
    return out;
}

struct BmpSpec
{
    std::uint32_t headerSize = 40;
    std::int32_t width = 1;
    std::int32_t height = 1;
    int bits = 24;
    std::uint32_t compression = 0;
    std::string inHeader; ///< after the first 40 bytes of the header (masks), padded to headerSize
    std::string afterHeader; ///< a palette, or the masks of a 40-byte header
    std::size_t gap = 0;
    std::string pixels;
};

std::string bmp(const BmpSpec& s)
{
    std::string info = le(s.headerSize, 4) + le(std::uint32_t(s.width), 4) + le(std::uint32_t(s.height), 4) + le(1, 2)
        + le(std::uint32_t(s.bits), 2) + le(s.compression, 4) + le(std::uint32_t(s.pixels.size()), 4) + le(2835, 4)
        + le(2835, 4) + le(0, 4) + le(0, 4);
    info += s.inHeader;
    info.resize(s.headerSize, '\0');
    const std::size_t offset = 14 + info.size() + s.afterHeader.size() + s.gap;
    const std::string body = info + s.afterHeader + std::string(s.gap, '\x5a') + s.pixels;
    return "BM" + le(std::uint32_t(14 + body.size()), 4) + le(0, 4) + le(std::uint32_t(offset), 4) + body;
}

} // namespace

TEST(ImageDecode, PngOfEveryKindDecodesToStraightAlpha)
{
    Result r = run(ImageFiles::Png);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.frames.size(), 1u);
    EXPECT_EQ(r.frames[0].width, 3);
    EXPECT_EQ(r.frames[0].height, 2);
    EXPECT_EQ(r.frames[0].delay, 0);
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 10, 20, 30, 255, 200, 100,
                                                            50, 255, 1, 2, 3, 4}));
    r = run(ImageFiles::Png16);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{0, 0, 0, 255, 255, 255, 255, 255}));
    r = run(ImageFiles::PngPalette);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{255, 0, 0, 255, 0, 0, 255, 0}));
}

TEST(ImageDecode, JpegIncludingAdobeCmyk)
{
    for (const std::string* file : {&ImageFiles::Jpeg, &ImageFiles::JpegCmyk}) {
        const Result r = run(*file);
        ASSERT_TRUE(r.ok) << r.error;
        ASSERT_EQ(r.frames.size(), 1u);
        EXPECT_EQ(r.frames[0].width, 8);
        EXPECT_EQ(r.frames[0].height, 8);
        EXPECT_TRUE(near(r.frames[0], 255, 0, 0)) << int(r.frames[0].rgba[0]) << ' ' << int(r.frames[0].rgba[1]) << ' '
                                                 << int(r.frames[0].rgba[2]);
    }
}

TEST(ImageDecode, GifAndWebPAnimationsGiveEachFrame)
{
    for (const std::string* file : {&ImageFiles::Gif, &ImageFiles::WebpAnimated}) {
        const Result r = run(*file);
        ASSERT_TRUE(r.ok) << r.error;
        ASSERT_EQ(r.frames.size(), 2u);
        for (const Frame& f : r.frames) {
            EXPECT_EQ(f.width, 4);
            EXPECT_EQ(f.height, 4);
            EXPECT_EQ(f.delay, 100);
        }
        EXPECT_EQ(r.frames[0].rgba, repeat({255, 0, 0, 255}, 16));
        EXPECT_EQ(r.frames[1].rgba, repeat({0, 0, 255, 255}, 16));
        // The sink stops the decoding.
        EXPECT_EQ(run(*file, 1).frames.size(), 1u);
    }
    const Result still = run(ImageFiles::Webp);
    ASSERT_TRUE(still.ok) << still.error;
    ASSERT_EQ(still.frames.size(), 1u);
    EXPECT_EQ(still.frames[0].rgba, repeat({255, 0, 0, 255}, 4));
}

TEST(ImageDecode, BmpOfEachDepthAndHeader)
{
    Result r = run(ImageFiles::Bmp);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}));

    // 8 bits with a palette of two, top-down (a negative height), with a
    // gap before the pixels. Index 5 is past the palette: black.
    BmpSpec s;
    s.bits = 8;
    s.width = 3;
    s.height = -2;
    s.afterHeader = le(0xFF0000, 4) + le(0x00FF00, 4); // 0x00RRGGBB: red, green
    s.gap = 7;
    s.pixels = std::string("\x00\x01\x05\x00", 4) + std::string("\x01\x00\x00\x00", 4);
    // biClrUsed: two colours.
    std::string file = bmp(s);
    file.replace(14 + 32, 4, le(2, 4));
    r = run(file);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 0, 255,
                                                            0, 255, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255}));

    // 1 bit, bottom-up: the last row in the file is the first.
    s = {};
    s.bits = 1;
    s.width = 9;
    s.height = 2;
    s.afterHeader = le(0x000000, 4) + le(0xFFFFFF, 4);
    s.pixels = std::string("\x80\x00\x00\x00", 4) + std::string("\x00\x80\x00\x00", 4);
    r = run(bmp(s));
    ASSERT_TRUE(r.ok) << r.error;
    const auto& px = r.frames[0].rgba;
    EXPECT_EQ(px[8 * 4], 255); // first row, ninth pixel
    EXPECT_EQ(px[0], 0);
    EXPECT_EQ(px[9 * 4], 255); // second row, first pixel
    EXPECT_EQ(px[9 * 4 + 8 * 4], 0);

    // 4 bits.
    s = {};
    s.bits = 4;
    s.width = 2;
    s.afterHeader = std::string(4 * 16, '\0');
    s.afterHeader.replace(4 * 15, 4, le(0x123456, 4));
    s.pixels = std::string("\xF0\x00\x00\x00", 4);
    r = run(bmp(s));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{0x12, 0x34, 0x56, 255, 0, 0, 0, 255}));

    // 16 bits without masks: 5-5-5, with no alpha.
    s = {};
    s.bits = 16;
    s.pixels = le(0x7C00, 2) + std::string(2, '\0');
    r = run(bmp(s));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{255, 0, 0, 255}));

    // 16 bits with 5-6-5 masks after a 40-byte header.
    s.compression = 3;
    s.afterHeader = le(0xF800, 4) + le(0x07E0, 4) + le(0x001F, 4);
    s.pixels = le(0x07E0, 2) + std::string(2, '\0');
    r = run(bmp(s));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{0, 255, 0, 255}));

    // 32 bits without masks: no alpha, whatever the fourth byte holds.
    s = {};
    s.bits = 32;
    s.pixels = le(0x10203040, 4);
    r = run(bmp(s));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{0x20, 0x30, 0x40, 255}));

    // 32 bits with the masks and the alpha mask of a V5 header.
    s.headerSize = 124;
    s.compression = 3;
    s.inHeader = le(0x00FF0000, 4) + le(0x0000FF00, 4) + le(0x000000FF, 4) + le(0xFF000000, 4);
    r = run(bmp(s));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{0x20, 0x30, 0x40, 0x10}));

    // The OS/2 1.x header: 16-bit sides and a palette of three bytes an entry.
    std::string os2 = le(12, 4) + le(1, 2) + le(1, 2) + le(1, 2) + le(8, 2);
    const std::string os2Palette = std::string("\x30\x20\x10", 3) + std::string(3 * 255, '\0');
    const std::string os2Body = os2 + os2Palette + std::string("\x00\x00\x00\x00", 4);
    r = run("BM" + le(std::uint32_t(14 + os2Body.size()), 4) + le(0, 4) + le(std::uint32_t(14 + os2.size() + os2Palette.size()), 4) + os2Body);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{0x10, 0x20, 0x30, 255}));
}

TEST(ImageDecode, RefusesBmpsThatAreNotValid)
{
    BmpSpec s;
    s.pixels = std::string(4, '\0');
    // Run-length encoded.
    s.bits = 8;
    s.compression = 1;
    s.afterHeader = std::string(4 * 256, '\0');
    EXPECT_FALSE(run(bmp(s)).ok);
    // A depth that does not exist.
    s = {};
    s.bits = 7;
    s.pixels = std::string(4, '\0');
    EXPECT_FALSE(run(bmp(s)).ok);
    // A mask whose bits are apart.
    s = {};
    s.bits = 32;
    s.compression = 3;
    s.afterHeader = le(0x00FF00FF, 4) + le(0x0000FF00, 4) + le(0xFF, 4);
    s.pixels = std::string(4, '\0');
    EXPECT_FALSE(run(bmp(s)).ok);
    // Rows that the file does not hold.
    s = {};
    s.width = 2;
    s.height = 2;
    s.pixels = std::string(8, '\0');
    EXPECT_FALSE(run(bmp(s)).ok);
    s.pixels = std::string(16, '\0');
    EXPECT_TRUE(run(bmp(s)).ok);
    // A height of -2^31, which has no positive.
    std::string file = bmp(s);
    file.replace(22, 4, le(0x80000000, 4));
    EXPECT_FALSE(run(file).ok);
}

TEST(ImageDecode, ProbeReadsTheSizeFromTheHeader)
{
    const auto size = [](std::string_view bytes) {
        int w = 0;
        int h = 0;
        const Probe p = probe(bytes, w, h);
        return std::to_string(int(p)) + ':' + std::to_string(w) + 'x' + std::to_string(h);
    };
    const std::string image = std::to_string(int(Probe::Image));
    EXPECT_EQ(size(ImageFiles::Png), image + ":3x2");
    EXPECT_EQ(size(ImageFiles::Jpeg), image + ":8x8");
    EXPECT_EQ(size(ImageFiles::Gif), image + ":4x4");
    EXPECT_EQ(size(ImageFiles::Webp), image + ":2x2");
    EXPECT_EQ(size(ImageFiles::WebpAnimated), image + ":4x4");
    EXPECT_EQ(size(ImageFiles::Bmp), image + ":2x2");
    // A side past MaxSide: too large, with the size.
    std::string wide = ImageFiles::Png;
    wide.replace(16, 4, std::string("\x00\x00\x4e\x21", 4));
    EXPECT_EQ(size(wide), std::to_string(int(Probe::TooLarge)) + ":20001x2");
    EXPECT_FALSE(run(wide).ok);
    const std::string unknown = std::to_string(int(Probe::Unknown));
    EXPECT_EQ(size("not an image"), unknown + ":0x0");
    EXPECT_EQ(size(""), unknown + ":0x0");
    // Apple's CgBI: a chunk before IHDR.
    const std::string cgbi = ImageFiles::Png.substr(0, 8) + std::string("\x00\x00\x00\x04" "CgBI" "\x50\x00\x20\x06" "\x00\x00\x00\x00", 16)
        + ImageFiles::Png.substr(8);
    EXPECT_EQ(size(cgbi), unknown + ":0x0");
    EXPECT_FALSE(run(cgbi).ok);
}

TEST(ImageDecode, RefusesCutAndCorruptFiles)
{
    for (const std::string* file : {&ImageFiles::Png, &ImageFiles::Webp, &ImageFiles::WebpAnimated, &ImageFiles::Bmp}) {
        const Result r = run(file->substr(0, file->size() / 2));
        EXPECT_FALSE(r.ok) << file->size();
        EXPECT_FALSE(r.error.empty());
    }
    // A JPEG cut after its header shows grey where its data is missing, as
    // in browsers. One cut in its header does not decode.
    EXPECT_FALSE(run(ImageFiles::Jpeg.substr(0, 100)).ok);
    std::string png = ImageFiles::Png;
    png[png.size() - 20] ^= 0x55; // in the image data: its checksum fails
    EXPECT_FALSE(run(png).ok);
    EXPECT_FALSE(run("BM").ok);
    EXPECT_FALSE(run(std::string("RIFF\0\0\0\0WEBP", 12)).ok);
}
