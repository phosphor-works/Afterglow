// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// TIFF, which none of the codecs reads, as kitten icat reads it: the first
// image of the file, in strips or tiles, uncompressed or with LZW, Deflate
// or PackBits, and the horizontal predictor. Grey (1, 2, 4, 8 and 16 bits),
// palette (1, 2, 4 and 8 bits), RGB and CMYK (8 and 16 bits), with an
// associated or unassociated alpha. JPEG data, planes of their own, signed
// and floating-point samples and BigTIFF are refused.

#include "Formats.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <map>
#include <vector>

namespace AfterglowImage::detail {

namespace {

enum Tag : std::uint16_t {
    ImageWidth = 256,
    ImageLength = 257,
    BitsPerSample = 258,
    Compression = 259,
    Photometric = 262,
    StripOffsets = 273,
    SamplesPerPixel = 277,
    RowsPerStrip = 278,
    StripByteCounts = 279,
    PlanarConfiguration = 284,
    Predictor = 317,
    ColorMap = 320,
    TileWidth = 322,
    TileLength = 323,
    TileOffsets = 324,
    TileByteCounts = 325,
    ExtraSamples = 338,
    SampleFormat = 339,
    IccProfile = 34675,
};

enum Kind : std::uint32_t {
    WhiteIsZero = 0,
    BlackIsZero = 1,
    Rgb = 2,
    Palette = 3,
    Cmyk = 5,
};

/// Where the values of a field are, and their kind.
struct Field
{
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::size_t offset = 0;
};

/// The first image file directory: the fields of the first image.
class Directory
{
public:
    bool read(std::string_view bytes)
    {
        m_bytes = bytes;
        if (bytes.size() < 8)
            return false;
        if (bytes.starts_with(std::string_view("II*\0", 4))) {
            m_little = true;
        } else if (!bytes.starts_with(std::string_view("MM\0*", 4))) {
            return false;
        }
        const std::size_t start = number(4, 4);
        if (start < 8 || start > bytes.size() - 2)
            return false;
        const std::size_t entries = number(start, 2);
        if (entries > (bytes.size() - start - 2) / 12)
            return false;
        for (std::size_t i = 0; i < entries; ++i) {
            const std::size_t entry = start + 2 + 12 * i;
            Field field;
            field.type = std::uint16_t(number(entry + 2, 2));
            field.count = number(entry + 4, 4);
            const std::uint64_t size = std::uint64_t(field.count) * typeSize(field.type);
            if (size == 0)
                continue;
            field.offset = size <= 4 ? entry + 8 : number(entry + 8, 4);
            // A field whose values are not in the file is left out: a field
            // that the image needs then refuses it.
            if (field.offset > bytes.size() || size > bytes.size() - field.offset)
                continue;
            m_fields.emplace(std::uint16_t(number(entry, 2)), field);
        }
        return true;
    }

    bool little() const
    {
        return m_little;
    }
    bool has(Tag tag) const
    {
        return m_fields.contains(tag);
    }
    std::uint32_t count(Tag tag) const
    {
        const auto it = m_fields.find(tag);
        return it == m_fields.end() ? 0 : it->second.count;
    }
    /// Value @p index of an integer field, or @p fallback.
    std::uint32_t value(Tag tag, std::uint32_t fallback = 0, std::uint32_t index = 0) const
    {
        const auto it = m_fields.find(tag);
        if (it == m_fields.end() || index >= it->second.count)
            return fallback;
        const Field& f = it->second;
        switch (f.type) {
        case 1:
        case 7:
            return at(m_bytes, f.offset + index);
        case 3:
            return number(f.offset + 2 * std::size_t(index), 2);
        case 4:
            return number(f.offset + 4 * std::size_t(index), 4);
        default:
            return fallback;
        }
    }
    std::string_view bytes(Tag tag) const
    {
        const auto it = m_fields.find(tag);
        if (it == m_fields.end() || typeSize(it->second.type) != 1)
            return {};
        return m_bytes.substr(it->second.offset, it->second.count);
    }
    std::uint32_t number(std::size_t index, int count) const
    {
        return m_little ? detail::little(m_bytes, index, count) : big(m_bytes, index, count);
    }

private:
    static std::uint64_t typeSize(std::uint16_t type)
    {
        switch (type) {
        case 1:
        case 2:
        case 6:
        case 7:
            return 1;
        case 3:
        case 8:
            return 2;
        case 4:
        case 9:
        case 11:
            return 4;
        case 5:
        case 10:
        case 12:
            return 8;
        default:
            return 0;
        }
    }

    std::string_view m_bytes;
    bool m_little = false;
    std::map<std::uint16_t, Field> m_fields;
};

/// PackBits: a count, then that many bytes, or one byte that many times.
void unpackBits(std::string_view in, std::vector<std::uint8_t>& out, std::size_t& size)
{
    size = 0;
    std::size_t pos = 0;
    while (pos < in.size() && size < out.size()) {
        const int n = std::int8_t(in[pos++]);
        if (n >= 0) {
            const std::size_t length = std::min({std::size_t(n) + 1, in.size() - pos, out.size() - size});
            std::copy_n(in.begin() + std::ptrdiff_t(pos), length, out.begin() + std::ptrdiff_t(size));
            pos += std::size_t(n) + 1;
            size += length;
        } else if (n != -128 && pos < in.size()) {
            const std::size_t length = std::min(std::size_t(1 - n), out.size() - size);
            std::fill_n(out.begin() + std::ptrdiff_t(size), length, std::uint8_t(in[pos++]));
            size += length;
        }
    }
}

bool inflateData(std::string_view in, std::vector<std::uint8_t>& out, std::size_t& size)
{
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK)
        return false;
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    stream.avail_in = uInt(std::min<std::size_t>(in.size(), UINT32_MAX));
    stream.next_out = out.data();
    stream.avail_out = uInt(out.size());
    const int result = inflate(&stream, Z_FINISH);
    size = out.size() - stream.avail_out;
    inflateEnd(&stream);
    // A stream that fills the block before its end, or whose data ends
    // before its end, gives what it holds: the caller judges whether the
    // rows are there, as libtiff does. The last strip has fewer rows than
    // the block.
    return result == Z_STREAM_END || result == Z_BUF_ERROR;
}

/// TIFF's LZW: codes of 9 to 12 bits, most significant bit first, which
/// grow a code early (at 511 for 10 bits, not at 512 as in GIF). The old
/// LZW of before TIFF 6, least significant bit first, is refused. One
/// object reads each strip or tile: its tables are set up once, as an
/// image can have many small tiles.
class Lzw
{
public:
    Lzw()
    {
        for (std::uint32_t i = 0; i < 256; ++i) {
            m_suffix[i] = m_first[i] = std::uint8_t(i);
            m_length[i] = 1;
        }
    }

    bool unpack(std::string_view in, std::vector<std::uint8_t>& out, std::size_t& size)
    {
        constexpr std::uint32_t Clear = 256;
        constexpr std::uint32_t End = 257;
        size = 0;
        if (in.size() >= 2 && at(in, 0) == 0 && (at(in, 1) & 1))
            return false;
        // The codes from 258 are written before they are read: a code past
        // the next one is refused.
        std::uint32_t next = 258;
        int width = 9;
        int previous = -1;
        std::uint64_t bits = 0;
        int held = 0;
        std::size_t pos = 0;
        while (size < out.size()) {
            while (held < width && pos < in.size()) {
                bits = bits << 8 | at(in, pos++);
                held += 8;
            }
            if (held < width)
                break;
            const std::uint32_t code = std::uint32_t(bits >> (held - width)) & ((1u << width) - 1);
            held -= width;
            if (code == End)
                break;
            if (code == Clear) {
                next = 258;
                width = 9;
                previous = -1;
                continue;
            }
            if (code > next || (code == next && previous < 0))
                return false;
            if (previous >= 0 && next < 4096) {
                // The new string: the previous one and the first byte of this
                // one, which is its own first byte when it is the new one.
                m_prefix[next] = std::uint16_t(previous);
                m_suffix[next] = code == next ? m_first[std::size_t(previous)] : m_first[code];
                m_first[next] = m_first[std::size_t(previous)];
                m_length[next] = std::uint16_t(std::min<std::uint32_t>(m_length[std::size_t(previous)] + 1u, 4096));
                ++next;
            }
            // The string of the code, from its end back to its start.
            const std::size_t n = m_length[code];
            std::uint32_t c = code;
            for (std::size_t i = n; i-- > 0;) {
                m_string[i] = m_suffix[c];
                c = m_prefix[c];
            }
            const std::size_t copied = std::min(n, out.size() - size);
            std::copy_n(m_string.begin(), copied, out.begin() + std::ptrdiff_t(size));
            size += copied;
            previous = int(code);
            if (next + 1 >= (1u << width) && width < 12)
                ++width;
        }
        return true;
    }

private:
    std::array<std::uint16_t, 4096> m_prefix{};
    std::array<std::uint8_t, 4096> m_suffix{};
    std::array<std::uint8_t, 4096> m_first{};
    std::array<std::uint16_t, 4096> m_length{};
    std::array<std::uint8_t, 4096> m_string{};
};

/// The high byte, as libtiff and kitten read samples and palettes of 16
/// bits: a palette of 8-bit colours often holds them times 256.
std::uint8_t scale16(std::uint32_t value)
{
    return std::uint8_t(value >> 8);
}

/// The 16-bit sample @p i of @p row.
std::uint32_t sample16(const std::uint8_t* row, std::uint64_t i, bool little)
{
    return little ? std::uint32_t(row[2 * i] | row[2 * i + 1] << 8) : std::uint32_t(row[2 * i] << 8 | row[2 * i + 1]);
}

/// The photometric interpretation, with the default of a file without one.
std::uint32_t kindOf(const Directory& d, std::uint32_t samples)
{
    return d.value(Photometric, samples >= 3 ? Rgb : BlackIsZero);
}

} // namespace

Probe probeTiff(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    Directory directory;
    if (!directory.read(bytes))
        return Probe::Unknown;
    return sized(directory.value(ImageWidth), directory.value(ImageLength), width, height);
}

ColorSpace tiffColorSpace(std::string_view bytes)
{
    ColorSpace space;
    Directory directory;
    if (!directory.read(bytes))
        return space;
    const std::uint32_t kind = kindOf(directory, directory.value(SamplesPerPixel, 1));
    space.grey = kind == WhiteIsZero || kind == BlackIsZero;
    const std::string_view icc = directory.bytes(IccProfile);
    if (icc.size() <= MaxIcc)
        space.icc.assign(icc);
    return space;
}

bool decodeTiff(std::string_view bytes, const FrameSink& sink, const Options&, std::string& error, std::string&)
{
    int width = 0;
    int height = 0;
    Directory d;
    if (!d.read(bytes)) {
        error = "the TIFF has no valid header";
        return false;
    }
    if (refused(sized(d.value(ImageWidth), d.value(ImageLength), width, height), "TIFF", error))
        return false;
    const std::uint32_t samples = d.value(SamplesPerPixel, 1);
    const std::uint32_t bits = d.value(BitsPerSample, 1);
    // Before the samples are read one by one: a count from the file bounds
    // that loop.
    if (samples == 0 || samples > 8) {
        error = "the TIFF has " + std::to_string(samples) + " samples a pixel, which is not supported";
        return false;
    }
    for (std::uint32_t i = 0; i < samples; ++i) {
        if (d.value(BitsPerSample, bits, i) != bits || d.value(SampleFormat, 1, i) != 1) {
            error = "the TIFF has samples that are not unsigned integers of one size, which is not supported";
            return false;
        }
    }
    const std::uint32_t kind = kindOf(d, samples);
    std::uint32_t colours = 0;
    bool depthValid = false;
    switch (kind) {
    case WhiteIsZero:
    case BlackIsZero:
        colours = 1;
        depthValid = bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16;
        break;
    case Palette:
        colours = 1;
        depthValid =
            (bits == 1 || bits == 2 || bits == 4 || bits == 8) && samples == 1 && d.count(ColorMap) == 3u << bits;
        break;
    case Rgb:
        colours = 3;
        depthValid = bits == 8 || bits == 16;
        break;
    case Cmyk:
        colours = 4;
        depthValid = bits == 8 || bits == 16;
        break;
    default:
        error = "the TIFF has the photometric interpretation " + std::to_string(kind) + ", which is not supported";
        return false;
    }
    if (!depthValid || samples < colours) {
        error = "the TIFF has a depth of " + std::to_string(bits) + " bits in " + std::to_string(samples)
            + " samples, which is not supported";
        return false;
    }
    if (samples > 1 && d.value(PlanarConfiguration, 1) != 1) {
        error = "the TIFF holds its samples in planes, which is not supported";
        return false;
    }
    // The first extra sample is alpha as libtiff takes it (TIFFRGBAImage):
    // associated (premultiplied) as 1, unassociated as 2, unspecified as
    // associated with 4 samples and more, and a fourth sample of RGB with
    // no ExtraSamples field as associated.
    bool alpha = false;
    bool premultiplied = false;
    if (samples > colours) {
        const std::uint32_t extra = d.value(ExtraSamples, kind == Rgb && samples == 4 ? 1 : 3);
        alpha = extra == 1 || extra == 2 || (extra == 0 && samples > 3);
        premultiplied = extra != 2;
    }
    const std::uint32_t compression = d.value(Compression, 1);
    if (compression != 1 && compression != 5 && compression != 8 && compression != 32946 && compression != 32773) {
        error = "the TIFF has the compression " + std::to_string(compression) + ", which is not supported";
        return false;
    }
    const std::uint32_t predictor = d.value(Predictor, 1);
    if (predictor != 1 && (predictor != 2 || bits < 8)) {
        error = "the TIFF has the predictor " + std::to_string(predictor) + ", which is not supported";
        return false;
    }

    // Strips are tiles as wide as the image.
    const bool tiled = d.has(TileWidth);
    const std::uint64_t blockWidth = tiled ? d.value(TileWidth) : std::uint64_t(width);
    const std::uint64_t blockHeight =
        tiled ? d.value(TileLength) : std::min<std::uint64_t>(d.value(RowsPerStrip, UINT32_MAX), std::uint64_t(height));
    const std::uint64_t across = blockWidth ? (std::uint64_t(width) + blockWidth - 1) / blockWidth : 0;
    const std::uint64_t down = blockHeight ? (std::uint64_t(height) + blockHeight - 1) / blockHeight : 0;
    // Each block is read whole, also where it is past the image: the pixels
    // of the blocks are bounded by those of the image, so that the memory
    // and the work on pixels are. Tiles no larger than the image cover at
    // most four times its pixels, small images take tiles of a megapixel,
    // and tiles of up to 512 pixels a side fit any image, a narrow one too.
    const std::uint64_t blockPixels = across * blockWidth * down * std::min(blockHeight, std::uint64_t(height));
    const std::uint64_t edgePixels = (std::uint64_t(width) + std::min<std::uint64_t>(blockWidth, 512))
        * (std::uint64_t(height) + std::min<std::uint64_t>(blockHeight, 512));
    if (blockWidth == 0 || blockHeight == 0 || blockWidth > std::uint64_t(MaxSide)
        || blockHeight > std::uint64_t(MaxSide)
        || blockPixels
            > std::max(4 * std::uint64_t(width) * std::uint64_t(height) + (std::uint64_t(1) << 20), edgePixels)) {
        error = "the TIFF has strips or tiles of no valid size";
        return false;
    }
    const Tag offsets = tiled ? TileOffsets : StripOffsets;
    const Tag counts = tiled ? TileByteCounts : StripByteCounts;
    if (d.count(offsets) < across * down || (d.count(counts) < across * down && compression != 1)) {
        error = "the TIFF does not say where its data is";
        return false;
    }
    const std::uint64_t rowBytes = (blockWidth * samples * bits + 7) / 8;
    // The rows of a block that the image has: a tile taller than the image
    // holds no more.
    std::vector<std::uint8_t> block(std::size_t(rowBytes * std::min(blockHeight, std::uint64_t(height))));
    std::vector<unsigned char> rgba(std::size_t(width) * std::size_t(height) * 4);
    std::vector<std::array<std::uint8_t, 3>> palette;
    if (kind == Palette) {
        const std::uint32_t size = 1u << bits;
        palette.resize(size);
        for (std::uint32_t i = 0; i < size; ++i) {
            palette[i] = {scale16(d.value(ColorMap, 0, i)), scale16(d.value(ColorMap, 0, size + i)),
                          scale16(d.value(ColorMap, 0, 2 * size + i))};
        }
    }
    const std::uint32_t maximum = (1u << std::min<std::uint32_t>(bits, 16)) - 1;
    Lzw lzw;
    // The blocks can share their data, and codes that make no bytes (LZW
    // clears, empty deflate blocks, PackBits no-ops) let each block scan all
    // of it: the input that the blocks scan is bounded by the file and by the
    // bytes they make. A writer's block is less than twice its bytes, with a
    // small header.
    const std::uint64_t inputBound = bytes.size() + 2 * across * rowBytes * std::uint64_t(height) + 64 * across * down;
    std::uint64_t scanned = 0;

    for (std::uint64_t index = 0; index < across * down; ++index) {
        const std::uint64_t x0 = index % across * blockWidth;
        const std::uint64_t y0 = index / across * blockHeight;
        const std::uint64_t rows = std::min(blockHeight, std::uint64_t(height) - y0);
        const std::size_t needed = std::size_t(rowBytes * rows);
        const std::size_t offset = d.value(offsets, 0, std::uint32_t(index));
        const std::size_t length = d.count(counts) > index ? d.value(counts, 0, std::uint32_t(index)) : needed;
        if (offset >= bytes.size()) {
            error = "the TIFF is truncated";
            return false;
        }
        const std::string_view in = bytes.substr(offset, length);
        scanned += compression == 1 ? std::min(in.size(), block.size()) : in.size();
        if (scanned > inputBound) {
            error = "the TIFF has corrupt data";
            return false;
        }
        std::size_t size = 0;
        bool read = true;
        switch (compression) {
        case 1:
            size = std::min(in.size(), block.size());
            std::copy_n(in.begin(), size, block.begin());
            break;
        case 5:
            read = lzw.unpack(in, block, size);
            break;
        case 8:
        case 32946:
            read = inflateData(in, block, size);
            break;
        case 32773:
            unpackBits(in, block, size);
            break;
        }
        if (!read || size < needed) {
            error = compression == 1 ? "the TIFF is truncated" : "the TIFF has corrupt data";
            return false;
        }
        for (std::uint64_t y = 0; y < rows; ++y) {
            std::uint8_t* row = block.data() + y * rowBytes;
            // The predictor stores each sample as its difference from the
            // same sample of the pixel before.
            if (predictor == 2 && bits == 8) {
                for (std::uint64_t i = samples; i < blockWidth * samples; ++i)
                    row[i] = std::uint8_t(row[i] + row[i - samples]);
            } else if (predictor == 2) {
                for (std::uint64_t i = samples; i < blockWidth * samples; ++i) {
                    const std::uint32_t value =
                        (sample16(row, i, d.little()) + sample16(row, i - samples, d.little())) & 0xFFFF;
                    row[2 * i] = std::uint8_t(d.little() ? value : value >> 8);
                    row[2 * i + 1] = std::uint8_t(d.little() ? value >> 8 : value);
                }
            }
            const auto sample = [&](std::uint64_t i) -> std::uint32_t {
                if (bits == 8)
                    return row[i];
                if (bits == 16)
                    return sample16(row, i, d.little());
                const std::uint64_t bit = i * bits;
                return std::uint32_t(row[bit / 8] >> (8 - bits - bit % 8)) & maximum;
            };
            const auto eight = [&](std::uint32_t value) {
                return bits == 16 ? scale16(value)
                    : bits == 8   ? std::uint8_t(value)
                                  : std::uint8_t(value * 255 / maximum);
            };
            const std::uint64_t columns = std::min(blockWidth, std::uint64_t(width) - x0);
            for (std::uint64_t x = 0; x < columns; ++x) {
                const std::uint64_t s = x * samples;
                unsigned char* out = rgba.data() + ((y0 + y) * std::uint64_t(width) + x0 + x) * 4;
                switch (kind) {
                case WhiteIsZero:
                case BlackIsZero: {
                    const std::uint8_t grey = eight(sample(s));
                    out[0] = out[1] = out[2] = kind == WhiteIsZero ? std::uint8_t(255 - grey) : grey;
                    break;
                }
                case Palette:
                    std::copy_n(palette[sample(s)].begin(), 3, out);
                    break;
                case Rgb:
                    for (int c = 0; c < 3; ++c)
                        out[c] = eight(sample(s + std::uint64_t(c)));
                    break;
                case Cmyk: {
                    const std::uint32_t k = 255 - eight(sample(s + 3));
                    for (int c = 0; c < 3; ++c)
                        out[c] = std::uint8_t((255 - eight(sample(s + std::uint64_t(c)))) * k / 255);
                    break;
                }
                }
                out[3] = alpha ? eight(sample(s + colours)) : 255;
                if (alpha && premultiplied) {
                    const std::uint32_t a = out[3];
                    for (int c = 0; c < 3; ++c)
                        out[c] = a == 0 ? 0 : std::uint8_t(std::min<std::uint32_t>(255, (out[c] * 255u + a / 2) / a));
                }
            }
        }
    }
    sink(rgba.data(), width, height, 0);
    return true;
}

} // namespace AfterglowImage::detail
