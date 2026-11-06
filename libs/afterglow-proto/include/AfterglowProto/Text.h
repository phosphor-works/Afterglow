// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Text helpers: UTF-8 decoding and encoding, the checks that the terminal
// runs on the fields that a program sends, and the filter and cell widths
// of the text that the clients of the library print.
namespace AfterglowProto {

/// Decodes the code point at @p i and advances @p i past it. Returns false,
/// with @p i unchanged, for a sequence that is not valid UTF-8 (overlong,
/// surrogate, truncated) and when @p i is at or past the end of @p s.
inline bool nextUtf8(std::string_view s, std::size_t& i, char32_t& cp)
{
    if (i >= s.size()) {
        return false;
    }
    const auto b = static_cast<unsigned char>(s[i]);
    const int extra = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0 || s.size() - i <= std::size_t(extra)) {
        return false;
    }
    cp = extra == 0 ? b : b & (0x3F >> extra);
    for (int k = 1; k <= extra; ++k) {
        const auto c = static_cast<unsigned char>(s[i + std::size_t(k)]);
        if ((c & 0xC0) != 0x80) {
            return false;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    static constexpr char32_t MinForLength[] = {0, 0x80, 0x800, 0x10000};
    if (cp < MinForLength[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return false;
    }
    i += std::size_t(extra) + 1;
    return true;
}

/// Appends @p cp as UTF-8. @p cp must be a code point: at most U+10FFFF.
inline void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

inline constexpr bool isControl(char32_t cp)
{
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F);
}

/// The controls of bidirectional text: the marks (ALM, LRM, RLM), the
/// embeddings and overrides (U+202A to U+202E) and the isolates (U+2066 to
/// U+2069). Shown as they are, they reorder the text around them.
inline constexpr bool isBidiControl(char32_t cp)
{
    return cp == 0x061C || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
}

/// @p text, safe to print on a terminal: each byte that is not UTF-8, each
/// C0, DEL and C1 control, each control of bidirectional text and U+2028 and
/// U+2029 become '?'. Text from a program, a script or a file name then
/// cannot move the cursor, change colours, break the line or reorder the
/// text around it.
inline std::string printable(std::string_view text)
{
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        char32_t cp = 0;
        if (!nextUtf8(text, i, cp)) {
            out += '?';
            i = start + 1;
        } else if (isControl(cp) || isBidiControl(cp) || cp == 0x2028 || cp == 0x2029) {
            out += '?';
        } else {
            out.append(text.substr(start, i - start));
        }
    }
    return out;
}

/// The cells that @p cp takes on a terminal: 0 for a combining mark, 2 for a
/// wide character (East Asian, most emoji), 1 for the others. The blocks are
/// approximate: the terminal's own tables are in libs/afterglow-vt, which a
/// client of this library does not have.
inline constexpr int cellWidth(char32_t cp)
{
    // Combining marks and zero width characters.
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF)
        || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0x200B && cp <= 0x200D)
        || (cp >= 0xFE00 && cp <= 0xFE0F)) {
        return 0;
    }
    // East Asian wide and full width characters, and the emoji blocks.
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF)
        || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF)
        || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F)
        || (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F)
        || (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

/// What isCleanText() lets through besides printable text.
namespace TextAllow {
inline constexpr unsigned Newline = 1; ///< LF, for text shown on several lines
inline constexpr unsigned LineSeparators = 2; ///< U+2028 and U+2029, which Qt breaks lines at
} // namespace TextAllow

/// Escape-code safe text from a program: valid UTF-8 with no C0, DEL or C1
/// control, so it cannot move a cursor or forge a line where it is shown.
inline bool isCleanText(std::string_view s, unsigned allow = 0)
{
    std::size_t i = 0;
    while (i < s.size()) {
        char32_t cp = 0;
        if (!nextUtf8(s, i, cp)) {
            return false;
        }
        if (cp == '\n' && (allow & TextAllow::Newline)) {
            continue;
        }
        if (isControl(cp) || ((cp == 0x2028 || cp == 0x2029) && !(allow & TextAllow::LineSeparators))) {
            return false;
        }
    }
    return true;
}

/// A protocol identifier stripped to [A-Za-z0-9_+.-] and 128 bytes, so it is
/// safe to echo back to the program (kitty's notification and clipboard ids).
inline std::string sanitizeId(std::string_view id)
{
    std::string out;
    for (char c : id.substr(0, 128)) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'
            || c == '+' || c == '.') {
            out += c;
        }
    }
    return out;
}

} // namespace AfterglowProto
