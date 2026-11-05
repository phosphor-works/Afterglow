// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Text.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace AfterglowVt::detail {

// The text helpers are shared with the protocol library.
using AfterglowProto::appendUtf8;
using AfterglowProto::isCleanText;
using AfterglowProto::isControl;
using AfterglowProto::nextUtf8;
using AfterglowProto::sanitizeId;
namespace TextAllow = AfterglowProto::TextAllow;

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
