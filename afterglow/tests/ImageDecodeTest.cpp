// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The images that a pane outside a session decodes with Qt
// (decodeBoundedImage): what they show must match what a session pane and
// aglow show (libs/afterglow-image), where Qt reads a file otherwise.

#include "ImageDecode.h"
#include "ImageFiles.h"

#include <AfterglowProto/Zlib.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace AfterglowImage::TestFiles;

namespace {

std::vector<std::uint8_t> decoded(std::string_view file, std::uint32_t& width, std::uint32_t& height)
{
    std::vector<std::uint8_t> rgba;
    if (!Afterglow::decodeBoundedImage(file, "", width, height, rgba)) return {};
    return rgba;
}

/// A PNG chunk with its length and CRC.
std::string chunk(std::string_view type, std::string_view data)
{
    const auto be = [](std::uint32_t v) { return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; };
    const std::string body = std::string(type) + std::string(data);
    return be(std::uint32_t(data.size())) + body + be(AfterglowProto::crc32Of(body));
}

std::string gammaChunk(std::uint32_t value)
{
    return chunk("gAMA", std::string{char(value >> 24), char(value >> 16), char(value >> 8), char(value)});
}

/// @p png with @p inserted before its chunk @p type.
std::string before(std::string png, std::string_view type, const std::string& inserted)
{
    png.insert(png.find(type) - 4, inserted);
    return png;
}

/// The red sample of a PNG of one pixel, or -1.
int red(const std::string& png)
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const std::vector<std::uint8_t> rgba = decoded(png, width, height);
    return rgba.size() == 4 ? rgba[0] : -1;
}

} // namespace

TEST(WindowImageDecode, APngGammaChunkAppliesAsInKitty)
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Linear light (gAMA 1.0) of 128 is 186 for the display, as libpng and
    // kitty make it.
    std::vector<std::uint8_t> rgba = decoded(PngGamma, width, height);
    ASSERT_EQ(rgba.size(), 4u);
    EXPECT_NEAR(rgba[0], 186, 1);
    EXPECT_EQ(rgba[3], 255);
    // Without a gAMA chunk the samples are as they are, 16 bits too.
    rgba = decoded(Png16Mid, width, height);
    ASSERT_EQ(rgba.size(), 4u);
    EXPECT_EQ(rgba[0], 128);
    // An sRGB chunk wins over a gAMA chunk.
    rgba = decoded(before(PngGamma, "gAMA", chunk("sRGB", std::string(1, '\0'))), width, height);
    ASSERT_EQ(rgba.size(), 4u);
    EXPECT_EQ(rgba[0], 128);
}

// The gAMA and sRGB chunks count as libpng 1.6.59 counts them, which the
// decoders of sessions and aglow use.
TEST(WindowImageDecode, TheGammaChunksCountAsInLibpng)
{
    // PngGamma: a sample of 128 with gAMA 1.0, which makes 186.
    const std::string noGamma = [] {
        std::string png = PngGamma;
        png.erase(png.find("gAMA") - 4, 16);
        return png;
    }();
    ASSERT_EQ(red(noGamma), 128);
    ASSERT_NEAR(red(PngGamma), 186, 1);
    // An sRGB chunk after the gAMA chunk wins too. One with a rendering
    // intent past 3 is not valid, and does not count.
    EXPECT_EQ(red(before(PngGamma, "IDAT", chunk("sRGB", std::string(1, '\0')))), 128);
    EXPECT_NEAR(red(before(PngGamma, "IDAT", chunk("sRGB", std::string(1, '\x04')))), 186, 1);
    // Not after a cICP chunk and a gAMA chunk: libpng keeps their gamma.
    const std::string cicp = chunk("cICP", std::string("\x01\x0d\x00\x01", 4));
    EXPECT_NEAR(red(before(before(PngGamma, "gAMA", cicp), "IDAT", chunk("sRGB", std::string(1, '\0')))), 186, 1);
    // A gAMA chunk after the image data does not count.
    EXPECT_EQ(red(before(noGamma, "IEND", gammaChunk(100000))), 128);
    // The first of two counts: 1/2.2 before 1.0 leaves the samples.
    EXPECT_EQ(red(before(PngGamma, "gAMA", gammaChunk(45455))), 128);
    // A value past 31 bits does not count, and the one after it does. A
    // value of 0 counts, as no gamma, and the one after it does not.
    EXPECT_NEAR(red(before(PngGamma, "gAMA", gammaChunk(0x80000000u))), 186, 1);
    EXPECT_EQ(red(before(PngGamma, "gAMA", gammaChunk(0))), 128);
    // Within 5% of the display's gamma, libpng makes no correction.
    EXPECT_EQ(red(before(noGamma, "IDAT", gammaChunk(45000))), 128);
}

TEST(WindowImageDecode, ATopDownBmpDecodes)
{
    // Bmp with its rows stored top first: a negative height in its header.
    std::string topDown = Bmp;
    const std::uint32_t height = 0u - 2u;
    for (int i = 0; i < 4; ++i) topDown[22 + std::size_t(i)] = char(height >> (8 * i) & 0xFF);
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    const std::vector<std::uint8_t> rgba = decoded(topDown, w, h);
    ASSERT_EQ(rgba.size(), 16u);
    EXPECT_EQ(w, 2u);
    EXPECT_EQ(h, 2u);
    // The file holds blue, white first: now the top row.
    EXPECT_EQ(rgba, (std::vector<std::uint8_t>{0, 0, 255, 255, 255, 255, 255, 255, 255, 0, 0, 255, 0, 255, 0, 255}));
}
