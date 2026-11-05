// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Text.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace AfterglowVt::detail {

// The text checks are shared with the protocol library.
using AfterglowProto::isCleanText;
using AfterglowProto::isControl;
using AfterglowProto::nextUtf8;
using AfterglowProto::sanitizeId;
namespace TextAllow = AfterglowProto::TextAllow;

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

} // namespace AfterglowVt::detail
