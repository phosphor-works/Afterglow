// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The image decoder of the daemon: afterglow-mux --decode-image, which
// decodeImage() runs for each PNG and iTerm2 image.

#include "ImageDecoder.h"
#include "ImageFiles.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>

using AfterglowMux::ImageFormats;
using namespace ImageFiles;

namespace {

struct Decoded
{
    bool ok = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

Decoded decode(std::string_view data, ImageFormats formats, const char* decoder = AFTERGLOW_MUX)
{
    Decoded d;
    d.ok = AfterglowMux::decodeImage(data, formats, d.width, d.height, d.rgba, decoder);
    return d;
}

std::vector<std::uint8_t> repeat(std::vector<std::uint8_t> pixel, int count)
{
    std::vector<std::uint8_t> out;
    for (int i = 0; i < count; ++i) out.insert(out.end(), pixel.begin(), pixel.end());
    return out;
}

} // namespace

TEST(MuxImageDecoder, APngDecodesToItsPixelsWithStraightAlpha)
{
    for (const ImageFormats formats : {ImageFormats::Png, ImageFormats::Common}) {
        const Decoded d = decode(Png, formats);
        ASSERT_TRUE(d.ok);
        EXPECT_EQ(d.width, 3u);
        EXPECT_EQ(d.height, 2u);
        const std::vector<std::uint8_t> expected{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0,
                                                 10, 20, 30, 255, 200, 100, 50, 255, 1, 2, 3, 4};
        EXPECT_EQ(d.rgba, expected);
    }
}

TEST(MuxImageDecoder, AnItermImageCanBeJpegGifWebPOrBmp)
{
    Decoded d = decode(Bmp, ImageFormats::Common);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.width, 2u);
    EXPECT_EQ(d.height, 2u);
    EXPECT_EQ(d.rgba, (std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}));
    // The first frame of an animation, as the window shows it.
    d = decode(Gif, ImageFormats::Common);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.width, 4u);
    EXPECT_EQ(d.height, 4u);
    EXPECT_EQ(d.rgba, repeat({255, 0, 0, 255}, 16));
    d = decode(WebpAnimated, ImageFormats::Common);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.rgba, repeat({255, 0, 0, 255}, 16));
    d = decode(Webp, ImageFormats::Common);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.rgba, repeat({255, 0, 0, 255}, 4));
    d = decode(Jpeg, ImageFormats::Common);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.width, 8u);
    EXPECT_EQ(d.height, 8u);
    ASSERT_EQ(d.rgba.size(), 8u * 8 * 4);
    for (std::size_t i = 0; i < d.rgba.size(); i += 4) {
        EXPECT_GT(d.rgba[i], 240) << i;
        EXPECT_LT(d.rgba[i + 1], 16) << i;
        EXPECT_LT(d.rgba[i + 2], 16) << i;
        EXPECT_EQ(d.rgba[i + 3], 255) << i;
    }
}

TEST(MuxImageDecoder, RefusesOtherFormatsAndBadFiles)
{
    // kitty's f=100 is PNG only.
    for (const std::string* file : {&Jpeg, &Gif, &Bmp, &Webp}) EXPECT_FALSE(decode(*file, ImageFormats::Png).ok);
    EXPECT_FALSE(decode("", ImageFormats::Common).ok);
    EXPECT_FALSE(decode("not an image at all", ImageFormats::Common).ok);
    EXPECT_FALSE(decode(Png.substr(0, 40), ImageFormats::Png).ok);
    // A side past what a terminal takes: refused from the header.
    std::string wide = Png;
    wide[16] = '\x00';
    wide[17] = '\x01';
    wide[18] = '\x00';
    wide[19] = '\x00';
    EXPECT_FALSE(decode(wide, ImageFormats::Png).ok);
    // Apple's CgBI: a chunk before IHDR.
    std::string cgbi = Png.substr(0, 8) + std::string("\x00\x00\x00\x04" "CgBI" "\x50\x00\x20\x06" "\x00\x00\x00\x00", 16) + Png.substr(8);
    EXPECT_FALSE(decode(cgbi, ImageFormats::Common).ok);
}

TEST(MuxImageDecoder, ADecoderThatFailsOrSendsNonsenseDecodesNothing)
{
    EXPECT_FALSE(decode(Png, ImageFormats::Png, "/nonexistent/afterglow-mux").ok);
    EXPECT_FALSE(decode(Png, ImageFormats::Png, "/bin/false").ok);
    // A decoder that sends the file back: its first bytes are no size that
    // the store takes.
    char dir[] = "/tmp/afterglow-mux-decoder-XXXXXX";
    ASSERT_NE(::mkdtemp(dir), nullptr);
    const std::string echo = std::string(dir) + "/echo";
    {
        std::ofstream script(echo);
        script << "#!/bin/sh\nexec cat\n";
    }
    ASSERT_EQ(::chmod(echo.c_str(), 0700), 0);
    EXPECT_FALSE(decode(Png, ImageFormats::Png, echo.c_str()).ok);
    std::filesystem::remove_all(dir);
    // The next decode works: nothing of the failures stays.
    EXPECT_TRUE(decode(Png, ImageFormats::Png).ok);
}
