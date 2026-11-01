// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

#include <AfterglowVt/Sixel.h>

#include <gtest/gtest.h>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {
std::uint32_t pixel(const ImageFrame& f, int x, int y)
{
    const std::uint8_t* p = &f.rgba[(std::size_t(y) * f.width + std::size_t(x)) * 4];
    return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
}
} // namespace

TEST(Sixel, DecodesColoursRepeatsAndBands)
{
    SixelPalette pal = defaultSixelPalette();
    ImageFrame f;
    // Register 1 = pure red (RGB percent), register 2 = green via HLS (hue 240 is green in DEC HLS).
    ASSERT_TRUE(decodeSixel("#1;2;100;0;0#2;1;240;50;100#1!3~-#2~~", 1, 0, pal, f));
    EXPECT_EQ(f.width, 3u);
    EXPECT_EQ(f.height, 12u);
    EXPECT_EQ(pixel(f, 0, 0), 0xFFFF0000u);
    EXPECT_EQ(pixel(f, 2, 5), 0xFFFF0000u);
    EXPECT_EQ(pixel(f, 1, 6), 0xFF00FF00u);
    EXPECT_EQ(pixel(f, 2, 6), 0u); // unpainted, P2=1: transparent
    EXPECT_EQ(pal[1], 0xFF0000u); // definitions update the palette
}

TEST(Sixel, BackgroundFillAndRasterSize)
{
    SixelPalette pal = defaultSixelPalette();
    ImageFrame f;
    ASSERT_TRUE(decodeSixel("\"1;1;4;2#0@", 0, 0x123456, pal, f)); // raster 4x2, one pixel of colour 0
    EXPECT_EQ(f.width, 4u);
    EXPECT_EQ(f.height, 2u);
    EXPECT_EQ(pixel(f, 3, 1), 0xFF123456u); // P2=0: background colour
    EXPECT_EQ(pixel(f, 0, 0), 0xFF000000u); // register 0 is black
    EXPECT_FALSE(decodeSixel("", 0, 0, pal, f));
}

TEST(Sixel, PlacedAtTheCursorWithFootsCursorRule)
{
    RecordingHost host;
    Terminal t(20, 10, host);
    t.resize(20, 10, 10, 20);
    t.feed("\x1b[2;3H\x1bPq\"1;1;25;50#1;2;0;0;100!25~-!25~-!25~-!25~-!25~-!25~-!25~-!25~-!25~\x1b\\");
    ASSERT_EQ(t.graphics().placements().size(), 1u);
    const Placement& p = t.graphics().placements()[0];
    EXPECT_EQ(p.col, 2);
    // 50 px tall in 20 px rows: 3 rows, so the cursor ends two rows down, at the image's column.
    EXPECT_EQ(t.screen().cursor.y, 3);
    EXPECT_EQ(t.screen().cursor.x, 2);
    t.feed("\x1b[?8452h\x1b[1;1H\x1bPq#1~~~\x1b\\"); // 3 px wide: one cell; cursor right of it
    EXPECT_EQ(t.screen().cursor.x, 1);
    t.feed("\x1b[?80h\x1b[5;5H\x1bPq#1~\x1b\\"); // DECSDM: top left, cursor unmoved
    EXPECT_EQ(t.screen().cursor.y, 4);
    EXPECT_EQ(t.graphics().placements().back().col, 0);
}

TEST(Sixel, ReportsAndAttributes)
{
    RecordingHost host;
    Terminal t(20, 10, host);
    t.resize(20, 10, 10, 20);
    t.feed("\x1b[?1;1S\x1b[?2;1S\x1b[c");
    EXPECT_EQ(host.written, "\x1b[?1;0;1024S\x1b[?2;0;200;200S\x1b[?65;4;6;22;28;52c");
}

TEST(Sixel, BlankRunsAndBandsAreBoundedAndOverflowRejectsWholeImage)
{
    auto palette = defaultSixelPalette();
    ImageFrame frame;
    EXPECT_FALSE(decodeSixel("~!10000?!10000?~", 1, 0, palette, frame));
    EXPECT_FALSE(decodeSixel("~" + std::string(2000, '-'), 1, 0, palette, frame));
    EXPECT_FALSE(decodeSixel("~!10000~", 1, 0, palette, frame));
}

TEST(Sixel, BlankRunPastTheWidthLimitIsRejectedWithNothingAfterIt)
{
    auto palette = defaultSixelPalette();
    ImageFrame frame;
    EXPECT_FALSE(decodeSixel("~!10000?", 1, 0, palette, frame));
    // readParams caps a count at 2^20: many such runs would overflow an int
    // position if the bound regressed.
    std::string runs = "~";
    for (int i = 0; i < 2100; ++i) {
        runs += "!1048576?";
    }
    EXPECT_FALSE(decodeSixel(runs + "~", 1, 0, palette, frame));
}

TEST(Sixel, PixelAreaIsCapped)
{
    auto palette = defaultSixelPalette();
    ImageFrame frame;
    // Both sides under 10,000, the area over 4096 x 4096.
    EXPECT_FALSE(decodeSixel("\"1;1;5000;4000#0~", 1, 0, palette, frame));
    EXPECT_TRUE(decodeSixel("\"1;1;4096;4096#0~", 1, 0, palette, frame));
    EXPECT_EQ(frame.width, 4096u);
    EXPECT_EQ(frame.height, 4096u);
}

TEST(Sixel, WideImageWithoutRasterAttributesDecodes)
{
    auto palette = defaultSixelPalette();
    ImageFrame frame;
    // Growing the width must not double the height: 1,025 x 6 fits easily.
    ASSERT_TRUE(decodeSixel("#1" + std::string(1025, '~'), 1, 0, palette, frame));
    EXPECT_EQ(frame.width, 1025u);
    EXPECT_EQ(frame.height, 6u);
    // 3,000 x 3,100 is within the cap as well.
    std::string tall = "#1";
    for (int band = 0; band < 516; ++band) {
        tall += "!3000~-";
    }
    tall += "!3000~"; // 517 bands: 3,102 rows
    ASSERT_TRUE(decodeSixel(tall, 1, 0, palette, frame));
    EXPECT_EQ(frame.width, 3000u);
    EXPECT_EQ(frame.height, 3102u);
}

TEST(Sixel, LastBandCountsOnlyItsLitRows)
{
    auto palette = defaultSixelPalette();
    ImageFrame frame;
    // 682 full bands reach row 4,092; the last band lights four rows: exactly 4,096.
    std::string data = "\"1;1;4096;4096#1";
    for (int band = 0; band < 682; ++band) {
        data += "!4096~-";
    }
    data += "!4096N";
    ASSERT_TRUE(decodeSixel(data, 1, 0, palette, frame));
    EXPECT_EQ(frame.height, 4096u);
    // An image the full 10,000 rows high, with one line feed after it.
    std::string tall = "#1";
    for (int band = 0; band < 1666; ++band) {
        tall += "~-";
    }
    tall += "N-";
    ASSERT_TRUE(decodeSixel(tall, 1, 0, palette, frame));
    EXPECT_EQ(frame.height, 10000u);
}

TEST(Sixel, RasterAttributesAloneGiveTheImageItsSize)
{
    auto palette = defaultSixelPalette();
    ImageFrame frame;
    ASSERT_TRUE(decodeSixel("\"1;1;8;8", 1, 0x123456, palette, frame));
    EXPECT_EQ(frame.width, 8u);
    EXPECT_EQ(frame.height, 8u);
    EXPECT_EQ(pixel(frame, 7, 7), 0u); // P2=1: transparent
    ASSERT_TRUE(decodeSixel("\"1;1;8;8", 0, 0x123456, palette, frame));
    EXPECT_EQ(pixel(frame, 7, 7), 0xFF123456u);
}

TEST(Sixel, RepaintingTheSameBandHasALimit)
{
    auto palette = defaultSixelPalette();
    ImageFrame frame;
    std::string data = "#1!10000~";
    for (int i = 0; i < 1200; ++i) {
        data += "$!10000~"; // 60,000 pixel writes each, for 8 bytes
    }
    EXPECT_FALSE(decodeSixel(data, 1, 0, palette, frame));
    // A few passes over the same pixels, as colour encoders do, are fine.
    EXPECT_TRUE(decodeSixel("#1!100~$#2!100~$#3!100~", 1, 0, palette, frame));
}
