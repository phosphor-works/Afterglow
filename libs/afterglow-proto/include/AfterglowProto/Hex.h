// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace AfterglowProto {

/// Lowercase hexadecimal, as kitty writes hashes and random ids.
inline std::string toHex(std::string_view bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        out += digits[std::uint8_t(c) >> 4];
        out += digits[std::uint8_t(c) & 15];
    }
    return out;
}

} // namespace AfterglowProto
