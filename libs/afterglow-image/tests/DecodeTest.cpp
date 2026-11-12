// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The decoders in this process: the formats, their sizes from the header,
// and the files that they refuse. aglow's icat_decode.py and the tests of
// afterglow-mux run them in their child processes.

#include "../src/Formats.h"
#include "ImageFiles.h"

#include <AfterglowImage/Decode.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace AfterglowImage;
using namespace AfterglowImage::TestFiles;

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

std::string text(const std::vector<std::uint8_t>& bytes)
{
    std::string out;
    for (const std::uint8_t byte : bytes) out += std::to_string(byte) + ' ';
    return out;
}

/// The one frame of a still image of @p width by @p height, or a failure
/// that says what the decoder gave.
::testing::AssertionResult still(const Result& r, int width, int height)
{
    if (!r.ok) return ::testing::AssertionFailure() << "not decoded: " << r.error;
    if (r.frames.size() != 1) return ::testing::AssertionFailure() << r.frames.size() << " frames";
    const Frame& f = r.frames[0];
    if (f.width != width || f.height != height || f.delay != 0 || f.rgba.size() != std::size_t(width) * std::size_t(height) * 4)
        return ::testing::AssertionFailure() << f.width << " by " << f.height << ", delay " << f.delay;
    if (!r.error.empty() || !r.warning.empty()) return ::testing::AssertionFailure() << "says: " << r.error << r.warning;
    return ::testing::AssertionSuccess();
}

/// A still image of @p width by @p height with the pixels @p rgba.
::testing::AssertionResult shows(std::string_view file, int width, int height, const std::vector<std::uint8_t>& rgba)
{
    const Result r = run(file);
    if (const auto size = still(r, width, height); !size) return size;
    if (r.frames[0].rgba != rgba) return ::testing::AssertionFailure() << "pixels " << text(r.frames[0].rgba);
    return ::testing::AssertionSuccess();
}

/// A still image of @p width by @p height with each pixel near @p r, @p g,
/// @p b (a lossy format) and with the alpha @p a.
::testing::AssertionResult showsNear(std::string_view file, int width, int height, int r, int g, int b, int a = 255)
{
    const Result result = run(file);
    if (const auto size = still(result, width, height); !size) return size;
    const auto& px = result.frames[0].rgba;
    for (std::size_t i = 0; i < px.size(); i += 4) {
        if (std::abs(px[i] - r) > 16 || std::abs(px[i + 1] - g) > 16 || std::abs(px[i + 2] - b) > 16 || px[i + 3] != a)
            return ::testing::AssertionFailure() << "pixel " << i / 4 << " is " << int(px[i]) << ' ' << int(px[i + 1]) << ' '
                                                 << int(px[i + 2]) << ' ' << int(px[i + 3]);
    }
    return ::testing::AssertionSuccess();
}

/// Not decoded, with an error that holds @p what.
::testing::AssertionResult refuses(std::string_view file, std::string_view what = {})
{
    const Result r = run(file);
    if (r.ok) return ::testing::AssertionFailure() << "decoded, " << r.frames.size() << " frames";
    if (!r.frames.empty()) return ::testing::AssertionFailure() << "gave a frame, then failed: " << r.error;
    if (r.error.empty()) return ::testing::AssertionFailure() << "no error text";
    if (r.error.find(what) == std::string::npos) return ::testing::AssertionFailure() << "error: " << r.error;
    return ::testing::AssertionSuccess();
}

std::string le(std::uint32_t value, int bytes)
{
    std::string out;
    for (int i = 0; i < bytes; ++i) out += char(value >> (8 * i) & 0xFF);
    return out;
}

std::string probed(std::string_view bytes)
{
    int w = 0;
    int h = 0;
    const Probe p = probe(bytes, w, h);
    const char* const kind = p == Probe::Image ? "image" : p == Probe::TooLarge ? "too large" : "unknown";
    return std::string(kind) + ' ' + std::to_string(w) + 'x' + std::to_string(h);
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

// GIF files, block by block. The global table has four colours: black, red,
// blue, white.
std::string gifScreen(int width, int height, int background = 0, const char* version = "GIF89a")
{
    return std::string(version) + le(std::uint32_t(width), 2) + le(std::uint32_t(height), 2) + std::string("\x81", 1) + char(background)
        + std::string("\x00", 1) + std::string("\x00\x00\x00\xff\x00\x00\x00\x00\xff\xff\xff\xff", 12);
}

/// A frame of one pixel of the colour @p index at @p left, @p top: a clear
/// code, the index and the end code, in codes of three bits.
std::string gifPixel(int left, int top, int index)
{
    return std::string("\x2c", 1) + le(std::uint32_t(left), 2) + le(std::uint32_t(top), 2) + le(1, 2) + le(1, 2) + std::string("\x00\x02\x02", 3)
        + char(0x44 | index << 3) + std::string("\x01\x00", 2);
}

const std::string GifTrailer("\x3b", 1);

/// The place of the marker @p marker (0xC2: SOF2) in a JPEG.
std::size_t marker(const std::string& jpeg, char marker, std::size_t from = 0)
{
    return jpeg.find(std::string("\xff", 1) + marker, from);
}

/// The first scan of a JPEG: from its SOS marker to the marker after its data.
std::pair<std::size_t, std::size_t> firstScan(const std::string& jpeg)
{
    const std::size_t start = marker(jpeg, '\xda');
    std::size_t end = start + 2;
    // In the data a 0xFF is followed by 0 or by a restart marker.
    while (end + 1 < jpeg.size()
           && !(std::uint8_t(jpeg[end]) == 0xFF && jpeg[end + 1] != 0 && (std::uint8_t(jpeg[end + 1]) < 0xD0 || std::uint8_t(jpeg[end + 1]) > 0xD7)))
        ++end;
    return {start, end};
}

} // namespace

TEST(ImageDecode, PngOfEveryKindDecodesToStraightAlpha)
{
    EXPECT_TRUE(shows(Png, 3, 2, PngPixels));
    EXPECT_TRUE(shows(Png16, 2, 1, {0, 0, 0, 255, 255, 255, 255, 255}));
    EXPECT_TRUE(shows(PngPalette, 2, 1, {255, 0, 0, 255, 0, 0, 255, 0}));
    EXPECT_TRUE(shows(PngGreyAlpha, 2, 1, {100, 100, 100, 255, 200, 200, 200, 64}));
    EXPECT_TRUE(shows(PngRgb, 2, 1, {10, 20, 30, 255, 200, 100, 50, 255}));
    EXPECT_TRUE(shows(PngGrey2, 4, 1, {0, 0, 0, 255, 85, 85, 85, 255, 170, 170, 170, 255, 255, 255, 255, 255}));
    EXPECT_TRUE(shows(PngGreyTrns, 2, 1, {50, 50, 50, 0, 60, 60, 60, 255}));
    EXPECT_TRUE(shows(PngRgbTrns, 2, 1, {1, 2, 3, 0, 4, 5, 6, 255}));
    EXPECT_TRUE(shows(PngInterlaced, 3, 3, {1, 1, 1, 255, 2, 2, 2, 255, 3, 3, 3, 255, 11, 11, 11, 255, 12, 12, 12, 255, 13, 13, 13, 255,
                                            21, 21, 21, 255, 22, 22, 22, 255, 23, 23, 23, 255}));
}

TEST(ImageDecode, PngSamplesOf16BitsAreNotMadeBrighter)
{
    // The samples are sRGB, as in 8 bits: half of the range is 128. Taken as
    // linear light it would be 186.
    EXPECT_TRUE(shows(Png16Mid, 1, 1, {128, 128, 128, 255}));
    // 0xc000 of 0xffff is 191.25 of 255.
    EXPECT_TRUE(shows(PngRgba16, 1, 1, {128, 64, 191, 128}));
    // A gAMA chunk applies, as in kitty: linear light of 128 is 186 in sRGB.
    EXPECT_TRUE(shows(PngGamma, 1, 1, {186, 186, 186, 255}));
}

TEST(ImageDecode, JpegOfEachKind)
{
    EXPECT_TRUE(showsNear(Jpeg, 8, 8, 255, 0, 0));
    EXPECT_TRUE(showsNear(JpegGrey, 8, 8, 128, 128, 128));
    // Adobe's CMYK: each colour sample times the black one. Without the
    // black, JpegCmykDark is (255, 127, 0).
    EXPECT_TRUE(showsNear(JpegCmyk, 8, 8, 255, 0, 0));
    EXPECT_TRUE(showsNear(JpegCmykDark, 8, 8, 191, 95, 0));
    // Progressive: a gradient, so only the corner is compared.
    const Result r = run(JpegProgressive);
    ASSERT_TRUE(still(r, 16, 16));
    const auto& px = r.frames[0].rgba;
    EXPECT_NEAR(px[0], 200, 16);
    EXPECT_NEAR(px[1], 100, 16);
    EXPECT_NEAR(px[2], 50, 16);
    EXPECT_EQ(px[px.size() - 1], 255);
}

TEST(ImageDecode, JpegWarningsShowThePictureAndErrorsDoNot)
{
    // A JFIF version that the decoder does not know is a warning in the header.
    std::string jfif2 = Jpeg;
    const std::size_t version = jfif2.find("JFIF") + 5;
    ASSERT_EQ(jfif2[version], 1);
    jfif2[version] = 2;
    EXPECT_TRUE(showsNear(jfif2, 8, 8, 255, 0, 0));
    // Data that ends early is a warning: every row comes, as in browsers.
    const Result cut = run(Jpeg.substr(0, Jpeg.size() - 6));
    ASSERT_TRUE(cut.ok) << cut.error;
    ASSERT_EQ(cut.frames.size(), 1u);
    EXPECT_EQ(cut.frames[0].width, 8);
    EXPECT_EQ(cut.frames[0].rgba.back(), 255);
    // One cut in its header does not decode.
    EXPECT_TRUE(refuses(Jpeg.substr(0, 100)));

    // A fatal error after a warning, before any row: the JFIF warning, the
    // first scan, then a second frame header. TurboJPEG still calls it a
    // warning, and no picture came.
    std::string broken = JpegProgressive;
    broken[broken.find("JFIF") + 5] = 2;
    const std::size_t frame = marker(broken, '\xc2');
    const std::string header = broken.substr(frame, 2 + (std::size_t(std::uint8_t(broken[frame + 2])) << 8 | std::uint8_t(broken[frame + 3])));
    broken = broken.substr(0, firstScan(broken).second) + header + std::string("\xff\xd9", 2);
    EXPECT_TRUE(refuses(broken));
}

TEST(ImageDecode, JpegLimits)
{
    // More scans than a picture needs: the first scan, 1,001 times.
    const auto [start, end] = firstScan(JpegProgressive);
    std::string scans = JpegProgressive.substr(0, end);
    for (int i = 0; i < 1001; ++i) scans += JpegProgressive.substr(start, end - start);
    scans += JpegProgressive.substr(end);
    EXPECT_TRUE(refuses(scans, "scans"));
    // The same file with a few scans more than its own decodes.
    std::string few = JpegProgressive.substr(0, end);
    for (int i = 0; i < 5; ++i) few += JpegProgressive.substr(start, end - start);
    few += JpegProgressive.substr(end);
    EXPECT_TRUE(run(few).ok);

    // 12 bits for each sample.
    std::string deep = Jpeg;
    const std::size_t frame = marker(deep, '\xc0');
    ASSERT_EQ(deep[frame + 4], 8);
    deep[frame + 4] = 12;
    EXPECT_TRUE(refuses(deep));
    // A side past MaxSide: 10,001 pixels wide.
    std::string wide = Jpeg;
    wide.replace(frame + 7, 2, std::string("\x27\x11", 2));
    EXPECT_EQ(probed(wide), "too large 10001x8");
    EXPECT_TRUE(refuses(wide, "side longer than 10000"));
}

TEST(ImageDecode, GifAndWebPAnimationsGiveEachFrame)
{
    for (const std::string* file : {&Gif, &WebpAnimated}) {
        SCOPED_TRACE(file == &Gif ? "GIF" : "WebP");
        const Result r = run(*file);
        EXPECT_TRUE(r.ok) << r.error;
        EXPECT_TRUE(r.warning.empty()) << r.warning;
        ASSERT_EQ(r.frames.size(), 2u);
        for (const Frame& f : r.frames) {
            EXPECT_EQ(f.width, 4);
            EXPECT_EQ(f.height, 4);
            EXPECT_EQ(f.delay, 100);
        }
        EXPECT_EQ(r.frames[0].rgba, repeat({255, 0, 0, 255}, 16));
        EXPECT_EQ(r.frames[1].rgba, repeat({0, 0, 255, 255}, 16));
        // The sink stops the decoding, which is no error.
        const Result one = run(*file, 1);
        EXPECT_TRUE(one.ok);
        EXPECT_TRUE(one.error.empty() && one.warning.empty()) << one.error << one.warning;
        EXPECT_EQ(one.frames.size(), 1u);
    }
}

TEST(ImageDecode, WebPOfEachKind)
{
    EXPECT_TRUE(shows(Webp, 2, 2, repeat({255, 0, 0, 255}, 4)));
    EXPECT_TRUE(shows(WebpAlpha, 2, 1, {255, 0, 0, 128, 0, 0, 255, 255}));
    EXPECT_TRUE(showsNear(WebpLossy, 8, 8, 200, 100, 50));
    EXPECT_TRUE(showsNear(WebpLossyAlpha, 8, 8, 200, 100, 50, 128));
}

TEST(ImageDecode, AnAnimationKeepsTheFramesBeforeItsDamage)
{
    // WebP: the image data of the second frame is not a lossless image.
    std::string webp = WebpAnimated;
    const std::size_t second = webp.find("ANMF", webp.find("ANMF") + 4);
    ASSERT_NE(second, std::string::npos);
    const std::size_t data = webp.find("VP8L", second) + 8;
    for (std::size_t i = data + 5; i < webp.size(); ++i) webp[i] = '\xff';
    Result r = run(webp);
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames.size(), 1u);
    EXPECT_FALSE(r.warning.empty());
    // The same damage in the first frame is an error.
    webp = WebpAnimated;
    const std::size_t first = webp.find("VP8L") + 8;
    for (std::size_t i = first + 5; i < second; ++i) webp[i] = '\xff';
    EXPECT_TRUE(refuses(webp));
    // A WebP that is cut short has no frame: its container must be whole.
    EXPECT_TRUE(refuses(WebpAnimated.substr(0, WebpAnimated.size() - 4)));

    // GIF: a block of an unknown kind after the first frame.
    r = run(gifScreen(1, 1) + gifPixel(0, 0, 1) + std::string("\x99", 1) + gifPixel(0, 0, 2) + GifTrailer);
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames.size(), 1u);
    EXPECT_FALSE(r.warning.empty());
    // A GIF that is cut short in the descriptor of its second frame, or in
    // the data of its first: the frames so far, and a warning.
    r = run(gifScreen(1, 1) + gifPixel(0, 0, 1) + std::string("\x2c\x00\x00", 3));
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames.size(), 1u);
    EXPECT_FALSE(r.warning.empty());
    const std::string whole = gifScreen(1, 1) + gifPixel(0, 0, 1);
    r = run(whole.substr(0, whole.size() - 2));
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames.size(), 1u);
    EXPECT_FALSE(r.warning.empty());
    // A GIF without its trailer is whole.
    r = run(whole);
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.warning.empty()) << r.warning;
}

TEST(ImageDecode, AnAnimationOfMoreThan24DaysStops)
{
    // 130 frames of the longest time that a WebP frame has: the sum passes
    // what the decoder counts in, after 128 frames.
    const std::size_t first = WebpAnimated.find("ANMF");
    std::string frame = WebpAnimated.substr(first, WebpAnimated.find("ANMF", first + 4) - first);
    frame.replace(8 + 12, 3, std::string("\xff\xff\xff", 3));
    std::string file = WebpAnimated.substr(0, first);
    for (int i = 0; i < 130; ++i) file += frame;
    file.replace(4, 4, le(std::uint32_t(file.size() - 8), 4));
    const Result r = run(file, 1000);
    EXPECT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.frames.size(), 128u);
    EXPECT_EQ(r.frames.back().delay, 0xFFFFFF);
    EXPECT_FALSE(r.warning.empty());
}

TEST(ImageDecode, GifCanvasAndBlocks)
{
    // The canvas starts transparent: the background colour (blue) does not
    // show where the first frame does not draw.
    EXPECT_TRUE(shows(gifScreen(2, 1, 2) + gifPixel(0, 0, 1) + GifTrailer, 2, 1, {255, 0, 0, 255, 0, 0, 0, 0}));
    EXPECT_TRUE(shows(gifScreen(2, 1, 2) + gifPixel(1, 0, 3) + GifTrailer, 2, 1, {0, 0, 0, 0, 255, 255, 255, 255}));
    // A frame outside the screen draws nothing.
    EXPECT_TRUE(shows(gifScreen(1, 1) + gifPixel(5, 5, 1) + GifTrailer, 1, 1, {0, 0, 0, 0}));
    // A graphic control extension whose first block is empty is skipped.
    EXPECT_TRUE(shows(gifScreen(1, 1) + std::string("\x21\xf9\x00", 3) + gifPixel(0, 0, 1) + GifTrailer, 1, 1, {255, 0, 0, 255}));
    // Its transparent index: the pixel stays as the canvas was.
    EXPECT_TRUE(shows(gifScreen(1, 1) + std::string("\x21\xf9\x04\x01\x00\x00\x01\x00", 8) + gifPixel(0, 0, 1) + GifTrailer, 1, 1, {0, 0, 0, 0}));
    // The older signature.
    EXPECT_TRUE(shows(gifScreen(1, 1, 0, "GIF87a") + gifPixel(0, 0, 3) + GifTrailer, 1, 1, {255, 255, 255, 255}));

    // No image, with and without a trailer.
    EXPECT_TRUE(refuses(gifScreen(1, 1) + GifTrailer, "no image"));
    EXPECT_TRUE(refuses(gifScreen(1, 1), "no image"));
    // Cut in the descriptor of its first frame, also without a global table.
    EXPECT_TRUE(refuses(gifScreen(1, 1) + std::string("\x2c\x00\x00", 3), "truncated"));
    const std::string bare = std::string("GIF89a") + le(1, 2) + le(1, 2) + std::string("\x00\x00\x00", 3);
    EXPECT_TRUE(refuses(bare + std::string("\x2c\x00\x00", 3), "truncated"));
    // Cut after the descriptor, after the least code size and after the
    // length of the first block: no pixel, so no frame.
    const std::string pixel = gifPixel(0, 0, 1);
    for (const std::size_t cut : {10, 11, 12}) EXPECT_TRUE(refuses(gifScreen(1, 1) + pixel.substr(0, cut), "truncated")) << cut;
    // A frame with no colour table at all.
    EXPECT_TRUE(refuses(bare + gifPixel(0, 0, 1) + GifTrailer, "color table"));
    // A screen with an empty side.
    EXPECT_EQ(probed(gifScreen(0, 0) + gifPixel(0, 0, 1) + GifTrailer), "unknown 0x0");
    EXPECT_TRUE(refuses(gifScreen(0, 0) + gifPixel(0, 0, 1) + GifTrailer));
}

TEST(ImageDecode, BmpOfEachDepthAndHeader)
{
    EXPECT_TRUE(shows(Bmp, 2, 2, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}));

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
    EXPECT_TRUE(shows(file, 3, 2, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 0, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255}));

    // 1 bit, bottom-up: the last row in the file is the first.
    s = {};
    s.bits = 1;
    s.width = 9;
    s.height = 2;
    s.afterHeader = le(0x000000, 4) + le(0xFFFFFF, 4);
    s.pixels = std::string("\x80\x00\x00\x00", 4) + std::string("\x00\x80\x00\x00", 4);
    const Result r = run(bmp(s));
    ASSERT_TRUE(still(r, 9, 2));
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
    EXPECT_TRUE(shows(bmp(s), 2, 1, {0x12, 0x34, 0x56, 255, 0, 0, 0, 255}));

    // 24 bits, top-down: the first row in the file is the first.
    s = {};
    s.height = -2;
    s.pixels = std::string("\x03\x02\x01\x00", 4) + std::string("\x06\x05\x04\x00", 4);
    EXPECT_TRUE(shows(bmp(s), 1, 2, {1, 2, 3, 255, 4, 5, 6, 255}));

    // 16 bits without masks: 5-5-5, with no alpha. Five bits widen to eight
    // by their own top bits: 16 of 31 is 132.
    s = {};
    s.bits = 16;
    s.pixels = le(0x7C00, 2) + std::string(2, '\0');
    EXPECT_TRUE(shows(bmp(s), 1, 1, {255, 0, 0, 255}));
    s.pixels = le(0x4210, 2) + std::string(2, '\0');
    EXPECT_TRUE(shows(bmp(s), 1, 1, {132, 132, 132, 255}));

    // 16 bits with 5-6-5 masks after a 40-byte header.
    s.compression = 3;
    s.afterHeader = le(0xF800, 4) + le(0x07E0, 4) + le(0x001F, 4);
    s.pixels = le(0x07E0, 2) + std::string(2, '\0');
    EXPECT_TRUE(shows(bmp(s), 1, 1, {0, 255, 0, 255}));

    // 32 bits without masks: no alpha, whatever the fourth byte holds.
    s = {};
    s.bits = 32;
    s.pixels = le(0x10203040, 4);
    EXPECT_TRUE(shows(bmp(s), 1, 1, {0x20, 0x30, 0x40, 255}));
    // But with a V4 or V5 header whose alpha mask is the top byte, as Qt
    // reads it. Another mask there gives no alpha.
    const std::string masks = le(0x00FF0000, 4) + le(0x0000FF00, 4) + le(0x000000FF, 4);
    for (const std::uint32_t header : {108u, 124u}) {
        s.headerSize = header;
        s.inHeader = masks + le(0xFF000000, 4);
        EXPECT_TRUE(shows(bmp(s), 1, 1, {0x20, 0x30, 0x40, 0x10})) << header;
        s.inHeader = masks + le(0, 4);
        EXPECT_TRUE(shows(bmp(s), 1, 1, {0x20, 0x30, 0x40, 255})) << header;
    }

    // 32 bits with the masks and the alpha mask of a V5 header.
    s.headerSize = 124;
    s.compression = 3;
    s.inHeader = masks + le(0xFF000000, 4);
    EXPECT_TRUE(shows(bmp(s), 1, 1, {0x20, 0x30, 0x40, 0x10}));
    // The same masks in a header of 56 bytes, which has an alpha mask too.
    s.headerSize = 56;
    EXPECT_TRUE(shows(bmp(s), 1, 1, {0x20, 0x30, 0x40, 0x10}));
    // A header of 52 bytes has the three colour masks and no alpha.
    s.headerSize = 52;
    s.inHeader = masks;
    EXPECT_TRUE(shows(bmp(s), 1, 1, {0x20, 0x30, 0x40, 255}));
    // A 40-byte header with four masks after it (compression 6).
    s = {};
    s.bits = 32;
    s.compression = 6;
    s.afterHeader = masks + le(0xFF000000, 4);
    s.pixels = le(0x10203040, 4);
    EXPECT_TRUE(shows(bmp(s), 1, 1, {0x20, 0x30, 0x40, 0x10}));
    // Masks of ten bits: the top eight of each.
    s.compression = 3;
    s.afterHeader = le(0x3FF00000, 4) + le(0x000FFC00, 4) + le(0x000003FF, 4);
    s.pixels = le(0x3FF00000 | 0x200 << 10 | 0x004, 4);
    EXPECT_TRUE(shows(bmp(s), 1, 1, {255, 128, 1, 255}));

    // The OS/2 1.x header: 16-bit sides and a palette of three bytes an entry.
    std::string os2 = le(12, 4) + le(1, 2) + le(1, 2) + le(1, 2) + le(8, 2);
    const std::string os2Palette = std::string("\x30\x20\x10", 3) + std::string(3 * 255, '\0');
    const std::string os2Body = os2 + os2Palette + std::string("\x00\x00\x00\x00", 4);
    EXPECT_TRUE(shows("BM" + le(std::uint32_t(14 + os2Body.size()), 4) + le(0, 4) + le(std::uint32_t(14 + os2.size() + os2Palette.size()), 4) + os2Body,
                      1, 1, {0x10, 0x20, 0x30, 255}));
}

TEST(ImageDecode, RefusesBmpsThatAreNotValid)
{
    BmpSpec s;
    s.pixels = std::string(4, '\0');
    // Run-length encoded, and embedded JPEG and PNG data.
    s.bits = 8;
    s.afterHeader = std::string(4 * 256, '\0');
    for (const std::uint32_t compression : {1u, 2u, 4u, 5u}) {
        s.compression = compression;
        EXPECT_TRUE(refuses(bmp(s), "compressed")) << compression;
    }
    // A palette that the file does not hold.
    s.compression = 0;
    EXPECT_TRUE(run(bmp(s)).ok);
    s.afterHeader.clear();
    s.pixels.clear();
    EXPECT_TRUE(refuses(bmp(s), "truncated"));
    // A depth that does not exist.
    s = {};
    s.bits = 7;
    s.pixels = std::string(4, '\0');
    EXPECT_TRUE(refuses(bmp(s), "depth"));
    // Masks for 24 bits.
    s = {};
    s.compression = 3;
    s.afterHeader = std::string(12, '\0');
    s.pixels = std::string(4, '\0');
    EXPECT_TRUE(refuses(bmp(s), "compressed"));
    // A mask whose bits are apart.
    s = {};
    s.bits = 32;
    s.compression = 3;
    s.afterHeader = le(0x00FF00FF, 4) + le(0x0000FF00, 4) + le(0xFF, 4);
    s.pixels = std::string(4, '\0');
    EXPECT_TRUE(refuses(bmp(s), "mask"));
    // Rows that the file does not hold.
    s = {};
    s.width = 2;
    s.height = 2;
    s.pixels = std::string(8, '\0');
    EXPECT_TRUE(refuses(bmp(s), "truncated"));
    s.pixels = std::string(16, '\0');
    const std::string whole = bmp(s);
    EXPECT_TRUE(run(whole).ok);
    // Pixels that start inside the header, or past the file.
    std::string file = whole;
    file.replace(10, 4, le(20, 4));
    EXPECT_TRUE(refuses(file, "truncated"));
    file.replace(10, 4, le(std::uint32_t(whole.size() + 1), 4));
    EXPECT_TRUE(refuses(file, "truncated"));

    // Headers that are no size: each is unknown to the probe, with no size.
    // A height of -2^31 has no positive.
    const auto header = [&whole](std::size_t at, std::uint32_t value) {
        std::string changed = whole;
        changed.replace(at, 4, le(value, 4));
        return changed;
    };
    for (const std::string& bad : {header(14, 30), header(14, 128), header(18, 0), header(18, 0xFFFFFFFF), header(22, 0), header(22, 0x80000000)}) {
        EXPECT_EQ(probed(bad), "unknown 0x0");
        EXPECT_TRUE(refuses(bad, "no valid header"));
    }
    // A side past MaxSide.
    EXPECT_EQ(probed(header(18, 10001)), "too large 10001x2");
    EXPECT_TRUE(refuses(header(18, 10001), "side longer than 10000"));
}

TEST(ImageDecode, FormatComesFromTheSignature)
{
    for (const std::string* file : {&Png, &Png16, &PngPalette, &PngInterlaced}) EXPECT_EQ(format(*file), Format::Png);
    for (const std::string* file : {&Jpeg, &JpegCmyk, &JpegProgressive, &JpegGrey}) EXPECT_EQ(format(*file), Format::Jpeg);
    for (const std::string* file : {&Webp, &WebpAnimated, &WebpLossy, &WebpLossyAlpha}) EXPECT_EQ(format(*file), Format::WebP);
    EXPECT_EQ(format(Gif), Format::Gif);
    EXPECT_EQ(format("GIF87a"), Format::Gif);
    EXPECT_EQ(format(Bmp), Format::Bmp);
    EXPECT_EQ(format("BM"), Format::Bmp);
    EXPECT_EQ(format("GIF88a"), Format::Unknown);
    EXPECT_EQ(format(std::string("\xff\xd8", 2)), Format::Unknown);
    EXPECT_EQ(format(std::string("RIFF\x04\0\0\0WAVE", 12)), Format::Unknown);
    EXPECT_EQ(format(std::string("RIFF\x04\0\0\0WEB", 11)), Format::Unknown);
    EXPECT_EQ(format(Png.substr(1)), Format::Unknown);
    EXPECT_EQ(format(""), Format::Unknown);
}

TEST(ImageDecode, ProbeReadsTheSizeFromTheHeader)
{
    EXPECT_EQ(probed(Png), "image 3x2");
    EXPECT_EQ(probed(Jpeg), "image 8x8");
    EXPECT_EQ(probed(JpegProgressive), "image 16x16");
    EXPECT_EQ(probed(Gif), "image 4x4");
    EXPECT_EQ(probed(Bmp), "image 2x2");
    // WebP has the size in three places: lossless, lossy and the canvas of
    // an extended file.
    EXPECT_EQ(probed(Webp), "image 2x2");
    EXPECT_EQ(probed(WebpLossy), "image 8x8");
    EXPECT_EQ(probed(WebpLossyAlpha), "image 8x8");
    EXPECT_EQ(probed(WebpAnimated), "image 4x4");
    // The header of a lossless WebP ends at 25 bytes, before that of the others.
    EXPECT_EQ(probed(Webp.substr(0, 25)), "image 2x2");
    EXPECT_EQ(probed(Webp.substr(0, 24)), "unknown 0x0");
    EXPECT_EQ(probed(WebpLossy.substr(0, 29)), "unknown 0x0");

    // A side past MaxSide: too large, with the size, and the decoder says so.
    std::string wide = Png;
    wide.replace(16, 4, std::string("\x00\x00\x4e\x21", 4));
    EXPECT_EQ(probed(wide), "too large 20001x2");
    EXPECT_TRUE(refuses(wide, "side longer than 10000"));
    wide = Gif;
    wide.replace(6, 2, le(10001, 2));
    EXPECT_EQ(probed(wide), "too large 10001x4");
    EXPECT_TRUE(refuses(wide, "side longer than 10000"));
    wide = WebpAnimated;
    wide.replace(24, 3, le(10000, 3)); // the canvas width, less one
    EXPECT_EQ(probed(wide), "too large 10001x4");
    EXPECT_TRUE(refuses(wide, "side longer than 10000"));

    // Not an image: no size, also when the header has one side.
    EXPECT_EQ(probed("not an image"), "unknown 0x0");
    EXPECT_EQ(probed(""), "unknown 0x0");
    std::string flat = Png;
    flat.replace(16, 4, std::string(4, '\0'));
    EXPECT_EQ(probed(flat), "unknown 0x0");
    EXPECT_TRUE(refuses(flat, "no valid header"));
    // Apple's CgBI: a chunk before IHDR.
    EXPECT_EQ(probed(cgbi()), "unknown 0x0");
    EXPECT_TRUE(refuses(cgbi(), "no valid header"));
}

TEST(ImageDecode, RefusesCutAndCorruptFiles)
{
    for (const std::string* file : {&Png, &Webp, &WebpLossy, &WebpAnimated, &Bmp}) {
        EXPECT_TRUE(refuses(file->substr(0, file->size() / 2))) << file->size();
    }
    // A lossy WebP whose alpha chunk has an unknown method, which libwebp
    // reports as a lack of memory: a bad file all the same.
    std::string alpha = WebpLossyAlpha;
    ASSERT_EQ(alpha.substr(30, 4), "ALPH");
    alpha[38] = 0x03;
    EXPECT_TRUE(refuses(alpha, "corrupt"));
    std::string png = Png;
    png[png.size() - 20] ^= 0x55; // in the image data: its checksum fails
    EXPECT_TRUE(refuses(png));
    EXPECT_TRUE(refuses("BM", "no valid header"));
    EXPECT_TRUE(refuses(std::string("RIFF\0\0\0\0WEBP", 12), "no valid header"));
    EXPECT_TRUE(refuses("not an image", "not an image"));
}

TEST(ImageDecode, TheTextOfACodecComesWithoutTheNameOfItsFunction)
{
    using AfterglowImage::detail::libraryMessage;
    EXPECT_EQ(libraryMessage("tj3Decompress8(): Image is too large"), "Image is too large");
    EXPECT_EQ(libraryMessage("png_image_finish_read: damaged PNG_IMAGE_VERSION"), "damaged PNG_IMAGE_VERSION");
    EXPECT_EQ(libraryMessage("IDAT: CRC error"), "IDAT: CRC error");
    EXPECT_EQ(libraryMessage("Corrupt JPEG data: 2 extraneous bytes"), "Corrupt JPEG data: 2 extraneous bytes");
}
