// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// netPBM, which none of the codecs reads: PBM, PGM and PPM, plain (P1 to P3)
// and raw (P4 to P6), and PAM (P7) with the tuple types of images. Samples
// up to a maximum of 65535 scale to 8 bits. A file of several images gives
// its first.

#include "Formats.h"

#include <algorithm>
#include <vector>

namespace AfterglowImage::detail {

namespace {

bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
bool digit(char c) { return c >= '0' && c <= '9'; }

struct Header
{
    char kind = 0; ///< '1' to '7'
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint32_t depth = 0; ///< samples of each pixel
    std::uint32_t maximum = 1;
    bool alpha = false;
    std::size_t data = 0; ///< where the samples start
};

/// Reads the numbers and the comments of P1 to P6 and the plain samples.
class Reader
{
public:
    Reader(std::string_view bytes, std::size_t pos)
        : m_bytes(bytes)
        , m_pos(pos)
    {
    }
    std::size_t pos() const { return m_pos; }
    /// Past white space and comments, which end at the end of their line.
    void skip()
    {
        while (m_pos < m_bytes.size()) {
            if (m_bytes[m_pos] == '#') {
                while (m_pos < m_bytes.size() && m_bytes[m_pos] != '\n' && m_bytes[m_pos] != '\r') ++m_pos;
            } else if (space(m_bytes[m_pos])) {
                ++m_pos;
            } else {
                break;
            }
        }
    }
    bool number(std::uint64_t& value)
    {
        skip();
        if (m_pos >= m_bytes.size() || !digit(m_bytes[m_pos])) return false;
        value = 0;
        while (m_pos < m_bytes.size() && digit(m_bytes[m_pos])) {
            value = value * 10 + std::uint64_t(m_bytes[m_pos++] - '0');
            if (value > 0xFFFFFFFFu) return false;
        }
        return true;
    }
    /// One digit of a plain PBM, where the samples need no space between them.
    bool bit(std::uint32_t& value)
    {
        skip();
        if (m_pos >= m_bytes.size() || (m_bytes[m_pos] != '0' && m_bytes[m_pos] != '1')) return false;
        value = std::uint32_t(m_bytes[m_pos++] - '0');
        return true;
    }
    /// The one white space character between the header and raw samples.
    bool separator()
    {
        if (m_pos >= m_bytes.size() || !space(m_bytes[m_pos])) return false;
        ++m_pos;
        return true;
    }

private:
    std::string_view m_bytes;
    std::size_t m_pos;
};

/// The header of a PAM: lines of a keyword and a value up to ENDHDR.
bool readPam(std::string_view bytes, Header& header)
{
    std::size_t pos = 3;
    std::uint64_t width = 0, height = 0, depth = 0, maximum = 0;
    std::string type;
    while (pos < bytes.size()) {
        std::size_t end = bytes.find('\n', pos);
        if (end == std::string_view::npos) return false;
        std::string_view line = bytes.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && space(line.front())) line.remove_prefix(1);
        while (!line.empty() && space(line.back())) line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const std::size_t gap = line.find_first_of(" \t");
        const std::string_view key = line.substr(0, gap);
        std::string_view value = gap == std::string_view::npos ? std::string_view() : line.substr(gap + 1);
        while (!value.empty() && space(value.front())) value.remove_prefix(1);
        if (key == "ENDHDR") {
            header.data = pos;
            break;
        }
        if (key == "TUPLTYPE") {
            // Values of several TUPLTYPE lines join with a space.
            if (!type.empty()) type += ' ';
            type += value;
            continue;
        }
        std::uint64_t* field = key == "WIDTH" ? &width : key == "HEIGHT" ? &height : key == "DEPTH" ? &depth : key == "MAXVAL" ? &maximum : nullptr;
        Reader reader(value, 0);
        if (!field || !reader.number(*field)) return false;
        reader.skip();
        if (reader.pos() != value.size()) return false;
    }
    if (header.data == 0 || maximum == 0 || maximum > 65535) return false;
    header.width = width;
    header.height = height;
    header.maximum = std::uint32_t(maximum);
    // The tuple types of images, and their depths. Without a type, the
    // depth says which.
    if (type.empty()) {
        if (depth < 1 || depth > 4) return false;
    } else if (type == "BLACKANDWHITE" || type == "GRAYSCALE") {
        if (depth != 1) return false;
    } else if (type == "BLACKANDWHITE_ALPHA" || type == "GRAYSCALE_ALPHA") {
        if (depth != 2) return false;
    } else if (type == "RGB") {
        if (depth != 3) return false;
    } else if (type == "RGB_ALPHA") {
        if (depth != 4) return false;
    } else {
        return false;
    }
    header.depth = std::uint32_t(depth);
    header.alpha = depth == 2 || depth == 4;
    return true;
}

bool readHeader(std::string_view bytes, Header& header)
{
    if (bytes.size() < 3 || bytes[0] != 'P' || bytes[1] < '1' || bytes[1] > '7' || !space(bytes[2])) return false;
    header.kind = bytes[1];
    if (header.kind == '7') return readPam(bytes, header);
    Reader reader(bytes, 2);
    if (!reader.number(header.width) || !reader.number(header.height)) return false;
    const bool bitmap = header.kind == '1' || header.kind == '4';
    std::uint64_t maximum = 1;
    if (!bitmap && (!reader.number(maximum) || maximum == 0 || maximum > 65535)) return false;
    header.maximum = std::uint32_t(maximum);
    header.depth = header.kind == '3' || header.kind == '6' ? 3 : 1;
    // A plain file reads on from here; a raw one has its samples after one
    // white space character.
    if (header.kind >= '4' && !reader.separator()) return false;
    header.data = reader.pos();
    return true;
}

} // namespace

Probe probePnm(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    Header header;
    if (!readHeader(bytes, header)) return Probe::Unknown;
    return sized(header.width, header.height, width, height);
}

bool decodePnm(std::string_view bytes, const FrameSink& sink, std::string& error)
{
    int width = 0;
    int height = 0;
    Header header;
    if (!readHeader(bytes, header)) {
        error = "the netPBM image has no valid header";
        return false;
    }
    if (refused(sized(header.width, header.height, width, height), "netPBM image", error)) return false;
    const std::size_t pixels = std::size_t(width) * std::size_t(height);
    const std::uint32_t maximum = header.maximum;
    const auto scale = [maximum](std::uint32_t value) {
        // A sample above the maximum is not valid: it shows as the maximum.
        return std::uint8_t((std::min(value, maximum) * 255 + maximum / 2) / maximum);
    };
    // The samples of each pixel in turn, as 8 bits.
    std::vector<std::uint8_t> samples(pixels * header.depth);
    const bool wide = maximum > 255;
    if (header.kind == '1' || header.kind == '2' || header.kind == '3') {
        Reader reader(bytes, header.data);
        for (std::uint8_t& sample : samples) {
            std::uint64_t value = 0;
            std::uint32_t bit = 0;
            const bool read = header.kind == '1' ? reader.bit(bit) : reader.number(value);
            if (!read) {
                error = "the netPBM image is truncated";
                return false;
            }
            // In a PBM, 1 is black.
            sample = header.kind == '1' ? std::uint8_t(bit ? 0 : 255) : scale(std::uint32_t(std::min<std::uint64_t>(value, maximum)));
        }
    } else if (header.kind == '4') {
        const std::size_t row = (std::size_t(width) + 7) / 8;
        if ((bytes.size() - header.data) / row < std::size_t(height)) {
            error = "the netPBM image is truncated";
            return false;
        }
        for (std::size_t y = 0; y < std::size_t(height); ++y) {
            for (std::size_t x = 0; x < std::size_t(width); ++x) {
                const bool black = (at(bytes, header.data + y * row + x / 8) >> (7 - x % 8)) & 1;
                samples[y * std::size_t(width) + x] = black ? 0 : 255;
            }
        }
    } else {
        const std::size_t size = wide ? 2 : 1;
        if ((bytes.size() - header.data) / size < samples.size()) {
            error = "the netPBM image is truncated";
            return false;
        }
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = scale(wide ? big(bytes, header.data + 2 * i, 2) : at(bytes, header.data + i));
        }
    }
    std::vector<unsigned char> rgba(pixels * 4);
    const bool grey = header.depth <= 2;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t* in = samples.data() + i * header.depth;
        unsigned char* out = rgba.data() + i * 4;
        out[0] = in[0];
        out[1] = grey ? in[0] : in[1];
        out[2] = grey ? in[0] : in[2];
        out[3] = header.alpha ? in[header.depth - 1] : 255;
    }
    sink(rgba.data(), width, height, 0);
    return true;
}

} // namespace AfterglowImage::detail
