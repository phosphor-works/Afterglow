// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The decoders in this process: the formats, their sizes from the header,
// and the files that they refuse. aglow's icat_decode.py and the tests of
// aglow-mux run them in their child processes.

#include "../src/Formats.h"
#include "ImageFiles.h"

#include <AfterglowImage/Decode.h>

#include <gtest/gtest.h>
#include <lcms2.h>

#include <cstdint>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <tuple>
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

Result run(std::string_view bytes, std::size_t maxFrames = 100, PngFrames pngFrames = PngFrames::Animation)
{
    Result r;
    r.ok = decode(bytes, [&](const unsigned char* rgba, int width, int height, int delay) {
        r.frames.push_back({width, height, delay, std::vector<std::uint8_t>(rgba, rgba + std::size_t(width) * std::size_t(height) * 4)});
        return r.frames.size() < maxFrames;
    }, r.error, r.warning, pngFrames);
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

/// A frame of @p width by @p height at @p left, @p top with the colours
/// @p indices in the order of its data: row by row, or pass by pass when
/// @p interlaced. A clear code comes before each index, so that the codes
/// keep three bits and the decoder adds no string.
std::string gifFrame(int left, int top, int width, int height, const std::vector<int>& indices, bool interlaced = false)
{
    std::vector<unsigned> codes;
    for (const int index : indices) {
        codes.push_back(4);
        codes.push_back(unsigned(index));
    }
    codes.push_back(5);
    std::string data;
    unsigned bits = 0;
    int count = 0;
    for (const unsigned code : codes) {
        bits |= code << count;
        count += 3;
        for (; count >= 8; count -= 8, bits >>= 8) data += char(bits & 0xFF);
    }
    if (count > 0) data += char(bits & 0xFF);
    std::string out = std::string("\x2c", 1) + le(std::uint32_t(left), 2) + le(std::uint32_t(top), 2) + le(std::uint32_t(width), 2)
        + le(std::uint32_t(height), 2) + char(interlaced ? 0x40 : 0) + std::string("\x02", 1);
    for (std::size_t i = 0; i < data.size(); i += 255) {
        const std::string block = data.substr(i, 255);
        out += char(block.size()) + block;
    }
    return out + std::string("\x00", 1);
}

/// RGBA pixels, one after the other.
std::vector<std::uint8_t> joined(std::initializer_list<std::vector<std::uint8_t>> pixels)
{
    std::vector<std::uint8_t> out;
    for (const auto& pixel : pixels) out.insert(out.end(), pixel.begin(), pixel.end());
    return out;
}

/// A graphic control extension: the next frame's disposal and time in
/// hundredths of a second.
std::string gifControl(int disposal, int delay)
{
    return std::string("\x21\xf9\x04", 3) + char(disposal << 2) + le(std::uint32_t(delay), 2) + std::string("\x00\x00", 2);
}

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

/// @p value as @p bytes bytes, big-endian.
std::string be(std::uint32_t value, int bytes)
{
    std::string out;
    for (int i = bytes; i-- > 0;) out += char(value >> (8 * i) & 0xFF);
    return out;
}

/// The CRC-32 of PNG chunks and gzip.
std::uint32_t crc(std::string_view bytes)
{
    std::uint32_t c = 0xFFFFFFFF;
    for (const char ch : bytes) {
        c ^= std::uint8_t(ch);
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

/// @p data as a zlib stream of stored blocks, which needs no deflate.
/// Without @p end, the stream stops after the data: no last block and no
/// checksum.
std::string zlibStored(std::string_view data, bool end = true)
{
    std::string out("\x78\x01", 2);
    std::string_view rest = data;
    do {
        const std::size_t n = std::min<std::size_t>(rest.size(), 65535);
        const bool last = end && n == rest.size();
        out += char(last ? 1 : 0) + le(std::uint32_t(n), 2) + le(std::uint32_t(~n & 0xFFFF), 2);
        out.append(rest.substr(0, n));
        rest.remove_prefix(n);
        if (last) break;
    } while (!rest.empty());
    if (!end) return out;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const char c : data) {
        a = (a + std::uint8_t(c)) % 65521;
        b = (b + a) % 65521;
    }
    return out + be(b << 16 | a, 4);
}

// PNG and APNG files, chunk by chunk, with 8-bit samples.
const std::string PngSignature("\x89PNG\r\n\x1a\n", 8);

std::string pngChunk(std::string_view type, std::string_view data)
{
    return be(std::uint32_t(data.size()), 4) + std::string(type) + std::string(data) + be(crc(std::string(type) + std::string(data)), 4);
}

/// The whole chunk @p type of @p png, the first of its kind.
std::string chunkOf(std::string_view png, std::string_view type)
{
    const std::size_t at = png.find(type);
    if (at == std::string_view::npos || at < 4) return {};
    std::size_t length = 0;
    for (std::size_t i = at - 4; i < at; ++i) length = length << 8 | std::uint8_t(png[i]);
    return std::string(png.substr(at - 4, 12 + length));
}

int channelsOf(int colourType) { return colourType == 6 ? 4 : colourType == 2 ? 3 : colourType == 4 ? 2 : 1; }

/// The image data of @p samples, @p width by @p height, each row with the
/// filter 0.
std::string imageData(int width, int height, int colourType, const std::vector<std::uint8_t>& samples)
{
    std::string rows;
    const std::size_t row = std::size_t(width * channelsOf(colourType));
    for (int y = 0; y < height; ++y) {
        rows += '\0';
        rows.append(samples.begin() + std::ptrdiff_t(std::size_t(y) * row), samples.begin() + std::ptrdiff_t(std::size_t(y + 1) * row));
    }
    return zlibStored(rows);
}

std::string ihdrOf(int width, int height, int colourType)
{
    return pngChunk("IHDR", be(std::uint32_t(width), 4) + be(std::uint32_t(height), 4) + char(8) + char(colourType) + std::string(3, '\0'));
}

/// A PNG of @p samples (RGBA for the colour type 6), with @p before
/// between its IHDR and its image data.
std::string pngOf(int width, int height, const std::vector<std::uint8_t>& samples, std::string_view before = {}, int colourType = 6)
{
    return PngSignature + ihdrOf(width, height, colourType) + std::string(before)
        + pngChunk("IDAT", imageData(width, height, colourType, samples)) + pngChunk("IEND", {});
}

struct ApngFrame
{
    int width = 1;
    int height = 1;
    int x = 0;
    int y = 0;
    int dispose = 0;
    int blend = 0;
    std::vector<std::uint8_t> samples;
};

std::string fcTL(std::uint32_t sequence, const ApngFrame& f)
{
    return pngChunk("fcTL", be(sequence, 4) + be(std::uint32_t(f.width), 4) + be(std::uint32_t(f.height), 4) + be(std::uint32_t(f.x), 4)
                                + be(std::uint32_t(f.y), 4) + be(10, 2) + be(100, 2) + char(f.dispose) + char(f.blend));
}

/// An APNG whose default image is its first frame, with @p before between
/// its IHDR and its acTL.
std::string apngOf(int width, int height, const std::vector<ApngFrame>& frames, std::string_view before = {}, int colourType = 6)
{
    std::string out = PngSignature + ihdrOf(width, height, colourType) + std::string(before)
        + pngChunk("acTL", be(std::uint32_t(frames.size()), 4) + be(0, 4));
    std::uint32_t sequence = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const ApngFrame& f = frames[i];
        out += fcTL(sequence++, f);
        const std::string data = imageData(f.width, f.height, colourType, f.samples);
        out += i == 0 ? pngChunk("IDAT", data) : pngChunk("fdAT", be(sequence++, 4) + data);
    }
    return out + pngChunk("IEND", {});
}

/// The pixel at @p x, @p y of a frame @p width wide.
std::vector<std::uint8_t> pixelAt(const Frame& frame, int x, int y)
{
    const auto at = frame.rgba.begin() + std::ptrdiff_t((std::size_t(y) * std::size_t(frame.width) + std::size_t(x)) * 4);
    return std::vector<std::uint8_t>(at, at + 4);
}

// TIFF files, field by field.
struct TiffField
{
    std::uint16_t tag = 0;
    std::uint16_t type = 3; ///< 1 BYTE, 3 SHORT, 4 LONG, 7 UNDEFINED
    std::vector<std::uint32_t> values;
};

/// A TIFF of @p fields and the strips (tiles with @p tiled) @p blocks, whose
/// offsets and byte counts it adds. With @p shared, every one of @p shared
/// blocks is the first.
std::string tiffOf(std::vector<TiffField> fields, const std::vector<std::string>& blocks, bool tiled = false, bool bigEndian = false,
                   std::size_t shared = 0)
{
    const auto number = [&](std::uint32_t value, int bytes) { return bigEndian ? be(value, bytes) : le(value, bytes); };
    std::string data;
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> counts;
    for (const std::string& block : blocks) {
        offsets.push_back(std::uint32_t(8 + data.size()));
        counts.push_back(std::uint32_t(block.size()));
        data += block;
    }
    if (shared) {
        offsets.assign(shared, offsets.at(0));
        counts.assign(shared, counts.at(0));
    }
    fields.push_back({std::uint16_t(tiled ? 324 : 273), 4, offsets});
    fields.push_back({std::uint16_t(tiled ? 325 : 279), 4, counts});
    std::sort(fields.begin(), fields.end(), [](const TiffField& a, const TiffField& b) { return a.tag < b.tag; });
    const std::size_t directory = 8 + data.size();
    std::string entries;
    std::string values;
    const std::size_t valuesAt = directory + 2 + 12 * fields.size() + 4;
    for (const TiffField& f : fields) {
        const int size = f.type == 3 ? 2 : f.type == 4 ? 4 : 1;
        std::string bytes;
        for (const std::uint32_t v : f.values) bytes += number(v, size);
        entries += number(f.tag, 2) + number(f.type, 2) + number(std::uint32_t(f.values.size()), 4);
        if (bytes.size() <= 4) {
            entries += bytes + std::string(4 - bytes.size(), '\0');
        } else {
            entries += number(std::uint32_t(valuesAt + values.size()), 4);
            values += bytes;
        }
    }
    return (bigEndian ? std::string("MM\0*", 4) : std::string("II*\0", 4)) + number(std::uint32_t(directory), 4) + data
        + number(std::uint32_t(fields.size()), 2) + entries + number(0, 4) + values;
}

/// The fields of an image of @p width by @p height with @p samples samples
/// of @p bits bits, the photometric interpretation @p kind, in strips of
/// @p rows rows, and @p more.
std::vector<TiffField> tiffImage(std::uint32_t width, std::uint32_t height, std::uint32_t samples, std::uint32_t bits, std::uint32_t kind,
                                 std::uint32_t rows, std::vector<TiffField> more = {})
{
    std::vector<TiffField> fields{{256, 4, {width}}, {257, 4, {height}}, {258, 3, std::vector<std::uint32_t>(samples, bits)},
                                  {262, 3, {kind}}, {277, 3, {samples}}, {278, 4, {rows}}};
    fields.insert(fields.end(), more.begin(), more.end());
    return fields;
}

std::string bytesOf(std::initializer_list<int> values)
{
    std::string out;
    for (const int v : values) out += char(v);
    return out;
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

namespace {
/// Each pixel of the one frame of @p bytes is one of @p want, in order and
/// repeated, within 1 of each sample: Little CMS against Pillow's rounds.
::testing::AssertionResult convertsTo(std::string_view bytes, int width, int height,
                                      const std::vector<std::array<int, 3>>& want)
{
    const Result r = run(bytes);
    if (!r.ok || r.frames.size() != 1 || r.frames[0].width != width || r.frames[0].height != height)
        return ::testing::AssertionFailure() << "decode: " << r.error;
    const auto& px = r.frames[0].rgba;
    for (std::size_t i = 0; i < px.size() / 4; ++i) {
        const auto& w = want[i % want.size()];
        for (int c = 0; c < 3; ++c) {
            if (std::abs(int(px[i * 4 + std::size_t(c)]) - w[std::size_t(c)]) > 1)
                return ::testing::AssertionFailure() << "pixel " << i << ": " << text({px.begin() + std::ptrdiff_t(i * 4), px.begin() + std::ptrdiff_t(i * 4 + 4)});
        }
        if (px[i * 4 + 3] != 255) return ::testing::AssertionFailure() << "pixel " << i << " lost its alpha";
    }
    return ::testing::AssertionSuccess();
}
} // namespace

TEST(ImageDecode, AnIccProfileConvertsToSrgb)
{
    // An Adobe RGB (1998) compatible profile in each format that carries one.
    EXPECT_TRUE(convertsTo(JpegAdobe, 8, 8, {{66, 151, 202}}));
    EXPECT_TRUE(convertsTo(WebpAdobe, 4, 4, {{66, 151, 203}}));
    // The PNG's gAMA does not apply too: the profile comes before it.
    EXPECT_TRUE(convertsTo(PngAdobe, 2, 1, {{66, 151, 203}, {0, 201, 0}}));
    // No profile that converts: the pixels stay.
    EXPECT_TRUE(convertsTo(PngBadIcc, 2, 1, {{100, 150, 200}, {10, 200, 30}}));
}

TEST(ImageDecode, ACicpChunkConvertsToSrgbBeforeAProfile)
{
    // Display P3 with the sRGB transfer, over an iCCP that it comes before.
    EXPECT_TRUE(convertsTo(PngCicpP3, 2, 1, {{83, 152, 205}, {0, 204, 0}}));
    // BT.2020 with PQ: 203 cd/m² is the white of sRGB.
    EXPECT_TRUE(convertsTo(PngCicpPq, 2, 1, {{181, 181, 181}, {0, 161, 230}}));
}

TEST(ImageDecode, JpegWarningsShowThePictureAndErrorsDoNot)
{
    // A JFIF version that the decoder does not know is a warning in the header.
    std::string jfif2 = Jpeg;
    const std::size_t version = jfif2.find("JFIF") + 5;
    ASSERT_EQ(jfif2[version], 1);
    jfif2[version] = 2;
    EXPECT_TRUE(showsNear(jfif2, 8, 8, 255, 0, 0));
    // Data that ends early is a warning of libjpeg-turbo: every row comes,
    // as in browsers, and decode() gives no warning for a still image.
    const Result cut = run(Jpeg.substr(0, Jpeg.size() - 6));
    ASSERT_TRUE(cut.ok) << cut.error;
    EXPECT_TRUE(cut.warning.empty()) << cut.warning;
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

    // A frame larger than the screen and partly on it: only its pixels on the
    // canvas land, and its rows still start where they should.
    const std::vector<std::uint8_t> none{0, 0, 0, 0};
    const std::vector<std::uint8_t> red{255, 0, 0, 255};
    const std::vector<std::uint8_t> blue{0, 0, 255, 255};
    const std::vector<std::uint8_t> white{255, 255, 255, 255};
    const std::vector<std::uint8_t> black{0, 0, 0, 255};
    EXPECT_TRUE(shows(gifScreen(2, 2) + gifFrame(1, 0, 2, 2, {1, 2, 3, 2}) + GifTrailer, 2, 2, joined({none, red, none, white})));
    // An interlaced frame: the data has rows 0, 2, 1 and 3 of four.
    EXPECT_TRUE(shows(gifScreen(1, 4) + gifFrame(0, 0, 1, 4, {1, 2, 3, 0}, true) + GifTrailer, 1, 4, joined({red, white, blue, black})));

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

TEST(ImageDecode, GifDisposalOfAFrame)
{
    const std::vector<std::uint8_t> none{0, 0, 0, 0};
    const std::vector<std::uint8_t> red{255, 0, 0, 255};
    const std::vector<std::uint8_t> blue{0, 0, 255, 255};
    const std::vector<std::uint8_t> white{255, 255, 255, 255};
    // Disposal 2: the frame's rectangle goes back to transparent before the
    // next frame.
    Result r = run(gifScreen(2, 1) + gifControl(2, 10) + gifFrame(0, 0, 1, 1, {1}) + gifControl(0, 10) + gifFrame(1, 0, 1, 1, {2})
                   + GifTrailer);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.warning.empty()) << r.warning;
    ASSERT_EQ(r.frames.size(), 2u);
    EXPECT_EQ(r.frames[0].delay, 100);
    EXPECT_EQ(r.frames[0].rgba, joined({red, none}));
    EXPECT_EQ(r.frames[1].rgba, joined({none, blue}));
    // Disposal 3: the canvas goes back to what it was before the frame.
    r = run(gifScreen(2, 1) + gifFrame(0, 0, 2, 1, {3, 3}) + gifControl(3, 10) + gifFrame(0, 0, 1, 1, {1}) + gifFrame(1, 0, 1, 1, {2})
            + GifTrailer);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.frames.size(), 3u);
    EXPECT_EQ(r.frames[0].rgba, joined({white, white}));
    EXPECT_EQ(r.frames[1].rgba, joined({red, white}));
    EXPECT_EQ(r.frames[2].rgba, joined({white, blue}));
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

TEST(ImageDecode, BmpWithRunLengthEncoding)
{
    // Black, red, green and blue, as BGRA.
    const std::string colours = bytesOf({0, 0, 0, 0, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 0});
    const std::vector<std::uint8_t> black{0, 0, 0, 255}, red{255, 0, 0, 255}, green{0, 255, 0, 255}, blue{0, 0, 255, 255};
    BmpSpec s;
    s.width = 4;
    s.height = 2;
    s.bits = 8;
    s.compression = 1;
    s.afterHeader = colours + std::string(4 * 252, '\0');
    // The bottom row: a run of two, then three indices as they are, of
    // which the row holds two, and the end of the row. The top row: a move
    // by one, a run of one and the end of the image. What no run reaches
    // has the first colour, as in Qt.
    s.pixels = bytesOf({2, 1, 0, 3, 2, 3, 1, 0, 0, 0, 0, 2, 1, 0, 1, 3, 0, 1});
    EXPECT_TRUE(shows(bmp(s), 4, 2, joined({black, blue, black, black, red, red, green, blue})));
    // Data that ends early ends the image.
    s.pixels = bytesOf({1, 1});
    EXPECT_TRUE(shows(bmp(s), 4, 2, joined({black, black, black, black, red, black, black, black})));
    // RLE4: a run of indices that alternate and a run of one, then four
    // indices as they are.
    s.height = 1;
    s.bits = 4;
    s.compression = 2;
    s.afterHeader = colours + std::string(4 * 12, '\0');
    s.pixels = bytesOf({3, 0x12, 1, 0x30, 0, 1});
    EXPECT_TRUE(shows(bmp(s), 4, 1, joined({red, green, red, blue})));
    s.pixels = bytesOf({0, 4, 0x12, 0x13, 0, 1});
    EXPECT_TRUE(shows(bmp(s), 4, 1, joined({red, green, red, blue})));
}

TEST(ImageDecode, RefusesBmpsThatAreNotValid)
{
    BmpSpec s;
    s.pixels = std::string(4, '\0');
    // Run-length encoding of the other depth, and embedded JPEG and PNG
    // data.
    s.bits = 8;
    s.afterHeader = std::string(4 * 256, '\0');
    for (const std::uint32_t compression : {2u, 4u, 5u}) {
        s.compression = compression;
        EXPECT_TRUE(refuses(bmp(s), "compressed")) << compression;
    }
    s.bits = 4;
    s.compression = 1;
    s.afterHeader = std::string(4 * 16, '\0');
    EXPECT_TRUE(refuses(bmp(s), "compressed"));
    s.bits = 8;
    s.afterHeader = std::string(4 * 256, '\0');
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

/// A still image of @p width by @p height whose pixels are TiffPattern.
::testing::AssertionResult showsPattern(std::string_view file, int width, int height)
{
    const Result r = run(file);
    if (const auto size = still(r, width, height); !size) return size;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* p = r.frames[0].rgba.data() + (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
            const std::uint32_t want = TiffPattern(x, y);
            if (p[0] != (want >> 16) || p[1] != ((want >> 8) & 255) || p[2] != (want & 255) || p[3] != 255) {
                return ::testing::AssertionFailure() << "at " << x << ", " << y << ": " << int(p[0]) << ' ' << int(p[1]) << ' ' << int(p[2])
                                                     << ' ' << int(p[3]);
            }
        }
    }
    return ::testing::AssertionSuccess();
}

/// @p file with the value of its TIFF field @p entry (the tag, type and
/// count, as they are in the file) replaced by @p value.
std::string withField(std::string file, std::string_view entry, std::string_view value)
{
    const std::size_t at = file.find(entry);
    EXPECT_NE(at, std::string::npos);
    if (at != std::string::npos) file.replace(at + entry.size(), value.size(), value);
    return file;
}

TEST(ImageDecode, TiffOfEachKind)
{
    EXPECT_TRUE(shows(TiffRgb, 3, 2, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 10, 20, 30, 255, 200, 100, 50, 255, 1, 2, 3, 255}));
    // LZW with codes of 9 to 12 bits and a clear code, the predictor of 8
    // and 16 bits, PackBits in a big-endian file, and edge tiles.
    EXPECT_TRUE(showsPattern(TiffLzw, 40, 36));
    EXPECT_TRUE(showsPattern(TiffDeflatePredictor, 8, 8));
    EXPECT_TRUE(showsPattern(TiffPackBitsBig, 8, 8));
    EXPECT_TRUE(showsPattern(TiffTiled, 20, 20));
    EXPECT_TRUE(shows(Tiff16Predictor, 2, 1, {128, 64, 192, 255, 255, 1, 0, 255}));
    // 1 bit, white is zero, rows of whole bytes.
    std::vector<std::uint8_t> bilevel(10 * 2 * 4, 255);
    for (const int i : {0, 3, 9, 15}) std::fill_n(bilevel.begin() + i * 4, 3, std::uint8_t(0));
    EXPECT_TRUE(shows(TiffBilevel, 10, 2, bilevel));
    EXPECT_TRUE(shows(TiffPalette, 2, 1, {0, 0, 255, 255, 255, 0, 0, 255}));
    // Unassociated alpha stays as it is.
    EXPECT_TRUE(shows(TiffRgba, 2, 1, {200, 100, 50, 128, 1, 2, 3, 0}));
    EXPECT_TRUE(shows(TiffCmyk, 2, 1, {255, 0, 0, 255, 127, 127, 127, 255}));
    // The ICC profile of the first image.
    EXPECT_TRUE(convertsTo(TiffAdobe, 2, 1, {{66, 151, 203}, {0, 201, 0}}));
}

TEST(ImageDecode, RefusesTiffsThatAreNotSupportedOrValid)
{
    // TiffRgb is little-endian: tag, type and count, then the value.
    const std::string compression("\x03\x01\x03\x00\x01\x00\x00\x00", 8);
    const std::string photometric("\x06\x01\x03\x00\x01\x00\x00\x00", 8);
    EXPECT_TRUE(refuses(withField(TiffRgb, compression, std::string("\x07\x00", 2)), "compression 7"));
    EXPECT_TRUE(refuses(withField(TiffRgb, photometric, std::string("\x06\x00", 2)), "photometric interpretation 6"));
    EXPECT_TRUE(refuses(TiffRgb.substr(0, TiffRgb.size() - 1), "truncated"));
    // A count of samples far past 8 (the fuzzer found one near 2^32):
    // refused at once, not after a loop over each sample.
    const std::string samples("\x15\x01\x03\x00\x01\x00\x00\x00", 8);
    EXPECT_TRUE(refuses(withField(TiffRgb, samples, std::string("\xff\xff", 2)), "65535 samples"));
    // The byte count of TiffLzw's strip, far less than its data.
    const std::string counts("\x17\x01\x04\x00\x01\x00\x00\x00", 8);
    EXPECT_TRUE(refuses(withField(TiffLzw, counts, std::string("\x00\x01\x00\x00", 4)), "corrupt"));
    EXPECT_TRUE(refuses(std::string("II*\0\x08\0\0\0", 8), "no valid header"));
    const std::string width("\x00\x01\x04\x00\x01\x00\x00\x00", 8);
    const std::string wide = withField(TiffRgb, width, le(10001, 4));
    EXPECT_EQ(probed(wide), "too large 10001x2");
    EXPECT_TRUE(refuses(wide, "side longer than 10000"));
}

TEST(ImageDecode, NetpbmOfEachKind)
{
    // Plain PBM: 1 is black, and the digits need no space between them.
    EXPECT_TRUE(shows("P1\n# a comment\n3 2\n1 0 1\n010", 3, 2,
                      {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255}));
    EXPECT_TRUE(shows("P2\n3 1\n15\n0 7 15\n", 3, 1, {0, 0, 0, 255, 119, 119, 119, 255, 255, 255, 255, 255}));
    EXPECT_TRUE(shows("P3 1 1 255\n10 20 30\n", 1, 1, {10, 20, 30, 255}));
    // Raw PBM: rows of whole bytes.
    std::vector<std::uint8_t> bitmap(10 * 4, 255);
    for (const int i : {0, 3, 9}) std::fill_n(bitmap.begin() + i * 4, 3, std::uint8_t(0));
    EXPECT_TRUE(shows(std::string("P4\n10 1\n\x90\x40", 10), 10, 1, bitmap));
    // Samples of 16 bits, big-endian.
    EXPECT_TRUE(shows(std::string("P5 2 1 65535\n\x80\x00\xff\xff", 17), 2, 1, {128, 128, 128, 255, 255, 255, 255, 255}));
    EXPECT_TRUE(shows("P6\n2 1\n255\n\x01\x02\x03\xfa\xfb\xfc", 2, 1, {1, 2, 3, 255, 250, 251, 252, 255}));
    EXPECT_TRUE(shows("P7\nWIDTH 2\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" + std::string("\x01\x02\x03\x80\x04\x05\x06\x00", 8),
                      2, 1, {1, 2, 3, 128, 4, 5, 6, 0}));
    // A PAM without a tuple type: its depth says which.
    EXPECT_TRUE(shows("P7\nWIDTH 1\nHEIGHT 1\nDEPTH 2\nMAXVAL 3\nENDHDR\n\x01\x03", 1, 1, {85, 85, 85, 255}));
    EXPECT_EQ(probed("P6\n640 480\n255\n"), "image 640x480");

    EXPECT_TRUE(refuses("P6\n2 1\n255\n\x01", "truncated"));
    EXPECT_TRUE(refuses("P2 2 1 255 1", "truncated"));
    EXPECT_TRUE(refuses("P7\nWIDTH 1\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE CMYK\nENDHDR\n\x01\x02\x03\x04", "no valid header"));
    EXPECT_TRUE(refuses("P5 1 1 0\n\x01", "no valid header"));
    EXPECT_TRUE(refuses("P6 10001 1 255\n", "side longer than 10000"));
    // A plain maximum above 255, and samples that are not numbers or too
    // large.
    EXPECT_TRUE(shows("P2 2 1 1000\n0 500\n", 2, 1, {0, 0, 0, 255, 128, 128, 128, 255}));
    EXPECT_TRUE(refuses("P2 1 1 255\n99999999999", "not valid"));
    EXPECT_TRUE(refuses("P3 1 1 255\n1 2 x", "not valid"));
    // A raw PBM without its second row, a raw file without its samples,
    // which is refused before the memory for them, and a PAM tuple type of
    // another depth.
    EXPECT_TRUE(refuses(std::string("P4\n10 2\n\x90\x40", 10), "truncated"));
    EXPECT_TRUE(refuses("P6 10000 10000 255\n", "truncated"));
    EXPECT_TRUE(refuses("P7\nWIDTH 1\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB\nENDHDR\n\x01\x02\x03\x04", "no valid header"));
}

TEST(ImageDecode, ApngGivesEachFrameOnItsCanvas)
{
    const Result r = run(Apng);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.warning, "");
    ASSERT_EQ(r.frames.size(), 3u);
    const auto pixel = [&](std::size_t frame, int x, int y) {
        const std::uint8_t* p = r.frames[frame].rgba.data() + (std::size_t(y) * 4 + std::size_t(x)) * 4;
        return std::vector<std::uint8_t>(p, p + 4);
    };
    const std::vector<std::uint8_t> red{255, 0, 0, 255};
    EXPECT_EQ(r.frames[0].delay, 100);
    EXPECT_EQ(r.frames[1].delay, 50);
    EXPECT_EQ(r.frames[2].delay, 30);
    for (int i = 0; i < 16; ++i) EXPECT_EQ(pixel(0, i % 4, i / 4), red);
    // Blue at alpha 128 over red, in the 2 by 2 at (1, 1) only.
    EXPECT_EQ(pixel(1, 0, 0), red);
    EXPECT_EQ(pixel(1, 1, 1), (std::vector<std::uint8_t>{127, 0, 128, 255}));
    EXPECT_EQ(pixel(1, 2, 2), (std::vector<std::uint8_t>{127, 0, 128, 255}));
    EXPECT_EQ(pixel(1, 3, 3), red);
    // The second frame restored what it covered.
    EXPECT_EQ(pixel(2, 0, 0), (std::vector<std::uint8_t>{0, 255, 0, 255}));
    EXPECT_EQ(pixel(2, 1, 1), red);

    // A default image without an fcTL before it is not a frame.
    const Result hidden = run(ApngHiddenDefault);
    ASSERT_TRUE(hidden.ok) << hidden.error;
    ASSERT_EQ(hidden.frames.size(), 2u);
    EXPECT_EQ(hidden.frames[0].rgba, (std::vector<std::uint8_t>{255, 255, 255, 255, 255, 255, 255, 255}));
    EXPECT_EQ(hidden.frames[1].rgba, (std::vector<std::uint8_t>{0, 0, 0, 255, 0, 0, 0, 255}));

    // One frame after a default image that is not a frame: a still image
    // of that frame.
    const std::size_t second = ApngHiddenDefault.find("fcTL", ApngHiddenDefault.find("fcTL") + 4) - 4;
    EXPECT_TRUE(shows(ApngHiddenDefault.substr(0, second), 2, 1, {255, 255, 255, 255, 255, 255, 255, 255}));

    // The frames before a chunk out of order, and a warning.
    const Result bad = run(ApngBadSequence);
    ASSERT_TRUE(bad.ok) << bad.error;
    EXPECT_EQ(bad.frames.size(), 2u);
    EXPECT_NE(bad.warning.find("frame 3"), std::string::npos) << bad.warning;
    // A sink that says stop gets no more frames.
    EXPECT_EQ(run(Apng, 1).frames.size(), 1u);
}

TEST(ImageDecode, ApngStopsAtAFrameThatIsNotValid)
{
    const std::vector<std::uint8_t> red{255, 0, 0, 255, 255, 0, 0, 255};
    const ApngFrame first{2, 1, 0, 0, 0, 0, red};
    const ApngFrame blue{1, 1, 1, 0, 0, 0, {0, 0, 255, 255}};
    // Outside the canvas, without image data, and with data that is not
    // zlib: the frames before, and a warning that names the frame.
    ApngFrame outside = blue;
    outside.x = 2;
    Result r = run(apngOf(2, 1, {first, blue, outside}));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames.size(), 2u);
    EXPECT_EQ(r.warning, "the APNG stops at frame 3: it is outside the image");
    std::string empty = apngOf(2, 1, {first, blue});
    const std::size_t fdat = empty.find("fdAT") - 4;
    empty.erase(fdat, empty.find("IEND") - 4 - fdat);
    r = run(empty);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames.size(), 1u);
    EXPECT_EQ(r.warning, "the APNG stops at frame 2: it has no image data");
    std::string corrupt = apngOf(2, 1, {first, blue});
    const std::size_t data = corrupt.find("fdAT") + 8;
    corrupt.replace(data, 2, "\x78\x02", 2);
    corrupt.replace(corrupt.find("IEND") - 8, 4, be(crc(corrupt.substr(corrupt.find("fdAT"), corrupt.find("IEND") - 8 - corrupt.find("fdAT"))), 4));
    r = run(corrupt);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames.size(), 1u);
    EXPECT_NE(r.warning.find("the APNG stops at frame 2: "), std::string::npos) << r.warning;

    // An fdAT that holds only its sequence number counts in the order.
    std::string withEmpty = apngOf(2, 1, {first, blue, blue});
    const std::size_t third = withEmpty.find("fcTL", withEmpty.find("fdAT")) - 4;
    withEmpty.insert(third, pngChunk("fdAT", be(3, 4)));
    // The chunks after it move up by one.
    std::size_t at = third + 16;
    for (std::uint32_t sequence = 4; at < withEmpty.size(); ++sequence) {
        const std::string type = withEmpty.substr(at + 4, 4);
        if (type != "fcTL" && type != "fdAT") break;
        const std::size_t length = std::size_t(std::uint8_t(withEmpty[at + 2])) << 8 | std::uint8_t(withEmpty[at + 3]);
        std::string chunk = withEmpty.substr(at + 8, length);
        chunk.replace(0, 4, be(sequence, 4));
        withEmpty.replace(at, 12 + length, pngChunk(type, chunk));
        at += 12 + length;
    }
    r = run(withEmpty);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.frames.size(), 3u);
    EXPECT_EQ(r.warning, "");
}

TEST(ImageDecode, ApngWithAFirstFrameThatIsNotValidShowsItsDefaultImage)
{
    // The first frame, which is not the default image, is outside the
    // canvas: the default image, as the APNG specification asks.
    std::string file = ApngHiddenDefault;
    const std::size_t fctl = file.find("fcTL");
    std::string control = file.substr(fctl + 4, 26);
    control.replace(4, 4, be(3, 4));
    file.replace(fctl - 4, 38, pngChunk("fcTL", control));
    const std::vector<std::uint8_t> grey{128, 128, 128, 255, 128, 128, 128, 255};
    const Result r = run(file);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.frames.size(), 1u);
    EXPECT_EQ(r.frames[0].delay, 0);
    EXPECT_EQ(r.frames[0].rgba, grey);
    EXPECT_EQ(r.warning, "the APNG shows its default image, as its first frame is not valid: it is outside the image");
    // A default image that is not valid either: an error.
    std::string both = file;
    const std::size_t idat = both.find("IDAT");
    both[idat + 6] = char(both[idat + 6] ^ 0x55);
    EXPECT_TRUE(refuses(both));
}

TEST(ImageDecode, ApngGivesItsDefaultImageAsAStillImageWhenAsked)
{
    // As kitty shows a PNG (f=100): the default image, which the animation
    // does not show.
    const std::vector<std::uint8_t> grey{128, 128, 128, 255, 128, 128, 128, 255};
    Result r = run(ApngHiddenDefault, 100, PngFrames::DefaultImage);
    ASSERT_TRUE(still(r, 2, 1));
    EXPECT_EQ(r.frames[0].rgba, grey);
    // A default image that is the first frame: that frame alone.
    r = run(Apng, 100, PngFrames::DefaultImage);
    ASSERT_TRUE(still(r, 4, 4));
    EXPECT_EQ(r.frames[0].rgba, repeat({255, 0, 0, 255}, 16));
    EXPECT_TRUE(shows(Png, 3, 2, PngPixels));
    r = run(Png, 100, PngFrames::DefaultImage);
    ASSERT_TRUE(still(r, 3, 2));
    EXPECT_EQ(r.frames[0].rgba, PngPixels);
}

TEST(ImageDecode, ApngDisposesOfItsFramesOnTheCanvas)
{
    const std::vector<std::uint8_t> red = repeat({255, 0, 0, 255}, 2);
    const std::vector<std::uint8_t> clear{0, 0, 0, 0};
    // Background: the area of the frame is clear after it.
    Result r = run(apngOf(2, 1, {{2, 1, 0, 0, 1, 0, red}, {1, 1, 1, 0, 0, 0, {0, 0, 255, 255}}}));
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.frames.size(), 2u);
    EXPECT_EQ(pixelAt(r.frames[1], 0, 0), clear);
    EXPECT_EQ(pixelAt(r.frames[1], 1, 0), (std::vector<std::uint8_t>{0, 0, 255, 255}));
    // Previous, on the first frame: as background.
    r = run(apngOf(2, 1, {{2, 1, 0, 0, 2, 0, red}, {1, 1, 0, 0, 0, 0, {0, 255, 0, 255}}}));
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.frames.size(), 2u);
    EXPECT_EQ(pixelAt(r.frames[1], 0, 0), (std::vector<std::uint8_t>{0, 255, 0, 255}));
    EXPECT_EQ(pixelAt(r.frames[1], 1, 0), clear);
    // A palette and its transparency, which each frame's PNG takes from
    // the header, with a text chunk that none takes.
    const std::string palette = pngChunk("PLTE", bytesOf({255, 0, 0, 0, 0, 255})) + pngChunk("tRNS", bytesOf({255, 0}))
        + pngChunk("tEXt", "Comment" + std::string(1, '\0') + std::string(1000, 'x'));
    r = run(apngOf(2, 1, {{2, 1, 0, 0, 0, 0, {0, 1}}, {2, 1, 0, 0, 0, 0, {1, 0}}}, palette, 3));
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.frames.size(), 2u);
    EXPECT_EQ(r.frames[0].rgba, (std::vector<std::uint8_t>{255, 0, 0, 255, 0, 0, 255, 0}));
    EXPECT_EQ(r.frames[1].rgba, (std::vector<std::uint8_t>{0, 0, 255, 0, 255, 0, 0, 255}));
}

TEST(ImageDecode, PngHeaderIsTheFirstChunkWithItsOwnLength)
{
    // A colour chunk before IHDR: the probe refuses the file, and so does
    // the decoder, before it reads the colour chunk.
    const std::string before = Png.substr(0, 8) + pngChunk("cICP", bytesOf({12, 13, 0, 1})) + Png.substr(8);
    EXPECT_EQ(probed(before), "unknown 0x0");
    EXPECT_TRUE(refuses(before, "no valid header"));
    // An IHDR that is not 13 bytes long.
    std::string shortHeader = Png;
    shortHeader[11] = 12;
    EXPECT_EQ(probed(shortHeader), "unknown 0x0");
    EXPECT_TRUE(refuses(shortHeader, "no valid header"));
}

namespace {

/// A PNG chunk iCCP of @p profile, which zlib stores.
std::string iccpOf(std::string_view profile)
{
    return pngChunk("iCCP", "profile" + std::string(2, '\0') + zlibStored(profile));
}

/// The ICC profile of sRGB that Little CMS writes.
std::string srgbProfile()
{
    cmsHPROFILE profile = cmsCreate_sRGBProfile();
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(profile, nullptr, &size);
    std::string out(size, '\0');
    cmsSaveProfileToMem(profile, out.data(), &size);
    cmsCloseProfile(profile);
    return out;
}

/// The gAMA chunk of linear light: grey 128 is 186 in sRGB.
const std::string LinearGamma = pngChunk("gAMA", be(100000, 4));

} // namespace

TEST(ImageDecode, ColourChunksApplyByTheirOrderOfPrecedence)
{
    // cICP of sRGB: no conversion, and no gAMA after it.
    const std::string srgbCicp = pngChunk("cICP", bytesOf({1, 13, 0, 1}));
    EXPECT_TRUE(shows(pngOf(1, 1, {128}, srgbCicp + LinearGamma, 0), 1, 1, {128, 128, 128, 255}));
    // A cICP that has no profile here leaves the ICC profile, which comes
    // before gAMA.
    const std::string adobe = chunkOf(PngAdobe, "iCCP");
    ASSERT_FALSE(adobe.empty());
    const std::string unknownCicp = pngChunk("cICP", bytesOf({5, 13, 0, 1}));
    EXPECT_TRUE(convertsTo(pngOf(1, 1, {100, 150, 200, 255}, unknownCicp + adobe + LinearGamma), 1, 1, {{66, 151, 203}}));
    // An RGB profile on a grey image is dropped, as libpng drops it, and
    // the gAMA applies.
    EXPECT_TRUE(shows(pngOf(1, 1, {128}, adobe + LinearGamma, 0), 1, 1, {186, 186, 186, 255}));
    EXPECT_TRUE(shows(pngOf(1, 1, {128}, adobe, 0), 1, 1, {128, 128, 128, 255}));
    // An sRGB profile converts nothing, and the gAMA after it does not apply.
    detail::ColorSpace space;
    space.icc = srgbProfile();
    const detail::SrgbConverter converter(space);
    EXPECT_FALSE(converter.active());
    EXPECT_TRUE(converter.known());
    std::vector<std::uint8_t> ramp;
    for (int i = 0; i < 256; ++i) ramp.insert(ramp.end(), {std::uint8_t(i), std::uint8_t(255 - i), std::uint8_t(i / 2), 255});
    EXPECT_TRUE(shows(pngOf(256, 1, ramp, iccpOf(space.icc) + LinearGamma), 256, 1, ramp));
    // A WebP profile counts only with the ICC flag of its VP8X header.
    std::string webp = WebpAdobe;
    ASSERT_EQ(webp.substr(12, 4), "VP8X");
    webp[20] = char(webp[20] & ~0x20);
    EXPECT_TRUE(convertsTo(webp, 4, 4, {{100, 150, 200}}));
}

TEST(ImageDecode, ColourConversionKeepsAlphaAndConvertsEachFrame)
{
    // Display P3 with the sRGB transfer, as PngCicpP3: straight alpha stays.
    const std::string p3 = pngChunk("cICP", bytesOf({12, 13, 0, 1}));
    Result r = run(pngOf(2, 1, {100, 150, 200, 128, 10, 200, 30, 0}, p3));
    ASSERT_TRUE(still(r, 2, 1));
    EXPECT_NEAR(r.frames[0].rgba[0], 83, 1);
    EXPECT_NEAR(r.frames[0].rgba[1], 152, 1);
    EXPECT_NEAR(r.frames[0].rgba[2], 205, 1);
    EXPECT_EQ(r.frames[0].rgba[3], 128);
    EXPECT_EQ(r.frames[0].rgba[7], 0);
    // Each frame of an APNG, on a canvas in the colours of the file, and
    // without the gAMA of the file.
    r = run(apngOf(2, 1, {{2, 1, 0, 0, 0, 0, {100, 150, 200, 255, 100, 150, 200, 255}}, {1, 1, 1, 0, 0, 0, {10, 200, 30, 255}}},
                   p3 + LinearGamma));
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.frames.size(), 2u);
    for (const auto& [frame, x, want] : std::vector<std::tuple<int, int, std::array<int, 3>>>{
             {0, 0, {83, 152, 205}}, {0, 1, {83, 152, 205}}, {1, 0, {83, 152, 205}}, {1, 1, {0, 204, 0}}}) {
        const auto px = pixelAt(r.frames[std::size_t(frame)], x, 0);
        for (std::size_t c = 0; c < 3; ++c) EXPECT_NEAR(px[c], want[c], 1) << frame << ' ' << x;
    }
    // BT.709 with linear light, and BT.2020 with HLG: grey stays grey.
    EXPECT_TRUE(convertsTo(pngOf(1, 1, {128, 128, 128, 255}, pngChunk("cICP", bytesOf({1, 8, 0, 1}))), 1, 1, {{188, 188, 188}}));
    EXPECT_TRUE(convertsTo(pngOf(1, 1, {128, 128, 128, 255}, pngChunk("cICP", bytesOf({9, 18, 0, 1}))), 1, 1, {{138, 138, 138}}));
    // A JPEG profile in two APP2 segments, in the file in reverse order.
    std::string jpeg = JpegAdobe;
    const std::size_t app2 = marker(jpeg, '\xe2');
    const std::size_t length = std::size_t(std::uint8_t(jpeg[app2 + 2])) << 8 | std::uint8_t(jpeg[app2 + 3]);
    const std::string profile = jpeg.substr(app2 + 4 + 14, length - 2 - 14);
    const auto segment = [](int number, std::string_view part) {
        return std::string("\xff\xe2", 2) + be(std::uint32_t(2 + 14 + part.size()), 2) + std::string("ICC_PROFILE\0", 12) + char(number) + char(2)
            + std::string(part);
    };
    const std::size_t half = profile.size() / 2;
    jpeg.replace(app2, 2 + length, segment(2, std::string_view(profile).substr(half)) + segment(1, std::string_view(profile).substr(0, half)));
    EXPECT_TRUE(convertsTo(jpeg, 8, 8, {{66, 151, 202}}));
}

TEST(ImageDecode, TiffOfEachDepthOrderAndAlpha)
{
    // 16-bit grey in a big-endian file: the high byte.
    EXPECT_TRUE(shows(tiffOf(tiffImage(2, 1, 1, 16, 1, 1), {bytesOf({0x12, 0x34, 0xff, 0x00})}, false, true), 2, 1,
                      {0x12, 0x12, 0x12, 255, 0xff, 0xff, 0xff, 255}));
    // 2 bits of grey, black is zero, and 4 bits, white is zero.
    EXPECT_TRUE(shows(tiffOf(tiffImage(4, 1, 1, 2, 1, 1), {bytesOf({0x1b})}), 4, 1,
                      {0, 0, 0, 255, 85, 85, 85, 255, 170, 170, 170, 255, 255, 255, 255, 255}));
    EXPECT_TRUE(shows(tiffOf(tiffImage(2, 1, 1, 4, 0, 1), {bytesOf({0x3f})}), 2, 1, {204, 204, 204, 255, 0, 0, 0, 255}));
    // Associated alpha, from ExtraSamples 1, from an unspecified fourth
    // sample, and from RGB with four samples and no ExtraSamples, as libtiff
    // takes them.
    const std::string pixel = bytesOf({100, 50, 0, 128});
    const std::vector<std::uint8_t> straight{199, 100, 0, 128};
    EXPECT_TRUE(shows(tiffOf(tiffImage(1, 1, 4, 8, 2, 1, {{338, 3, {1}}}), {pixel}), 1, 1, straight));
    EXPECT_TRUE(shows(tiffOf(tiffImage(1, 1, 4, 8, 2, 1, {{338, 3, {0}}}), {pixel}), 1, 1, straight));
    EXPECT_TRUE(shows(tiffOf(tiffImage(1, 1, 4, 8, 2, 1), {pixel}), 1, 1, straight));
    // An unspecified second sample of grey is no alpha.
    EXPECT_TRUE(shows(tiffOf(tiffImage(1, 1, 2, 8, 1, 1, {{338, 3, {0}}}), {bytesOf({100, 128})}), 1, 1, {100, 100, 100, 255}));
    EXPECT_TRUE(shows(tiffOf(tiffImage(1, 1, 2, 8, 1, 1, {{338, 3, {2}}}), {bytesOf({100, 128})}), 1, 1, {100, 100, 100, 128}));
    // Deflate whose last strip has its rows but not the end of its stream,
    // which libtiff takes too.
    const std::vector<TiffField> deflate = tiffImage(1, 3, 1, 8, 1, 2, {{259, 3, {8}}});
    EXPECT_TRUE(shows(tiffOf(deflate, {zlibStored(bytesOf({10, 20})), zlibStored(bytesOf({30}), false)}), 1, 3,
                      {10, 10, 10, 255, 20, 20, 20, 255, 30, 30, 30, 255}));
    EXPECT_TRUE(refuses(tiffOf(deflate, {zlibStored(bytesOf({10, 20})), zlibStored({}, false)}), "corrupt"));
    // A tile far larger than a small image.
    const std::vector<TiffField> tiles{{256, 4, {1}}, {257, 4, {1}}, {258, 3, {8, 8, 8}}, {262, 3, {2}}, {277, 3, {3}},
                                       {322, 4, {1024}}, {323, 4, {1024}}};
    EXPECT_TRUE(shows(tiffOf(tiles, {bytesOf({1, 2, 3}) + std::string(3 * 1023, '\0')}, true), 1, 1, {1, 2, 3, 255}));
    // Tiles of 256 by 256 on an image of 32 columns, which cover more than
    // four times its pixels.
    const std::vector<TiffField> narrow{{256, 4, {32}}, {257, 4, {10000}}, {258, 3, {8}}, {262, 3, {1}}, {277, 3, {1}},
                                        {322, 4, {256}}, {323, 4, {256}}};
    EXPECT_TRUE(showsNear(tiffOf(narrow, {std::string(256 * 256, '\0')}, true, false, 40), 32, 10000, 0, 0, 0));
}

TEST(ImageDecode, RefusesTiffsOfEachKindThatIsNotSupported)
{
    const std::string pixel = bytesOf({1, 2, 3});
    EXPECT_TRUE(refuses(tiffOf(tiffImage(1, 1, 3, 8, 2, 1, {{339, 3, {2, 2, 2}}}), {pixel}), "not unsigned integers"));
    EXPECT_TRUE(refuses(tiffOf(tiffImage(1, 1, 3, 8, 2, 1, {{284, 3, {2}}}), {pixel}), "planes"));
    EXPECT_TRUE(refuses(tiffOf(tiffImage(1, 1, 3, 8, 2, 1, {{317, 3, {3}}}), {pixel}), "predictor 3"));
    EXPECT_TRUE(refuses(tiffOf(tiffImage(2, 1, 1, 4, 1, 1, {{317, 3, {2}}}), {bytesOf({0x12})}), "predictor 2"));
    EXPECT_TRUE(refuses(tiffOf(tiffImage(1, 1, 3, 12, 2, 1), {pixel}), "depth of 12 bits"));
    EXPECT_TRUE(refuses(tiffOf(tiffImage(1, 1, 1, 8, 3, 1, {{320, 3, std::vector<std::uint32_t>(3, 0)}}), {bytesOf({0})}), "depth"));
    EXPECT_TRUE(refuses(tiffOf(tiffImage(1, 1, 3, 8, 2, 1), {}), "does not say where"));
    // The LZW of before TIFF 6.
    EXPECT_TRUE(refuses(tiffOf(tiffImage(1, 1, 3, 8, 2, 1, {{259, 3, {5}}}), {bytesOf({0, 1, 2, 3})}), "corrupt"));
    // Tiles as wide as the longest side, on an image of one column: each
    // row of tiles would be read whole, from the same data.
    const std::vector<TiffField> tiles{{256, 4, {1}}, {257, 4, {10000}}, {258, 3, {8}}, {262, 3, {1}}, {277, 3, {1}},
                                       {322, 4, {10000}}, {323, 4, {16}}};
    EXPECT_TRUE(refuses(tiffOf(tiles, {std::string(10000 * 16, '\0')}, true, false, 625), "strips or tiles"));
    // Tiles of one pixel that share PackBits data of no-ops before their
    // byte: each tile would scan all of it.
    const std::string noOps = std::string(1000, '\x80') + bytesOf({0, 7});
    const auto packed = [](std::uint32_t side) {
        return std::vector<TiffField>{{256, 4, {side}}, {257, 4, {side}}, {258, 3, {8}}, {259, 3, {32773}}, {262, 3, {1}},
                                      {277, 3, {1}},    {322, 4, {1}},    {323, 4, {1}}};
    };
    EXPECT_TRUE(shows(tiffOf(packed(1), {noOps}, true), 1, 1, {7, 7, 7, 255}));
    EXPECT_TRUE(refuses(tiffOf(packed(64), {noOps}, true, false, 64 * 64), "corrupt"));
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
    for (const std::string* file : {&TiffRgb, &TiffPackBitsBig}) EXPECT_EQ(format(*file), Format::Tiff);
    EXPECT_EQ(format(std::string("II+\0", 4)), Format::Unknown); // BigTIFF
    for (const char* file : {"P1\n", "P6 ", "P7\n"}) EXPECT_EQ(format(file), Format::Pnm);
    EXPECT_EQ(format("P8\n"), Format::Unknown);
    EXPECT_EQ(format("P6x"), Format::Unknown);
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
