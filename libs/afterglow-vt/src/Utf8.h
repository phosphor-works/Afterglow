// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace AfterglowVt::detail {

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

/// Decodes the code point at @p i and advances @p i past it. Returns false
/// for a sequence that is not valid UTF-8 (overlong, surrogate, truncated).
inline bool nextUtf8(std::string_view s, std::size_t& i, char32_t& cp)
{
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

inline constexpr bool isControl(char32_t cp)
{
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F);
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

/// @p s cut to at most @p max bytes without splitting a code point.
inline std::string_view truncateUtf8(std::string_view s, std::size_t max)
{
    if (s.size() <= max) {
        return s;
    }
    std::size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
        --end;
    }
    return s.substr(0, end);
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

} // namespace AfterglowVt::detail
