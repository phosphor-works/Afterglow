// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace AfterglowVt::detail {

/// Decodes base64, skipping padding and anything outside the alphabet.
inline std::string base64Decode(std::string_view in)
{
    static constexpr auto table = [] {
        std::array<std::int8_t, 256> t{};
        t.fill(-1);
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            t[static_cast<unsigned char>(chars[i])] = std::int8_t(i);
        }
        return t;
    }();
    std::string out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        const std::int8_t v = table[static_cast<unsigned char>(ch)];
        if (v < 0) {
            continue;
        }
        acc = (acc << 6) | std::uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(char((acc >> bits) & 0xFF));
        }
    }
    return out;
}

inline std::string base64Encode(std::string_view in)
{
    static constexpr char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t n = (std::uint32_t(std::uint8_t(in[i])) << 16) | (std::uint32_t(std::uint8_t(in[i + 1])) << 8)
            | std::uint8_t(in[i + 2]);
        out += chars[(n >> 18) & 63];
        out += chars[(n >> 12) & 63];
        out += chars[(n >> 6) & 63];
        out += chars[n & 63];
    }
    if (i < in.size()) {
        std::uint32_t n = std::uint32_t(std::uint8_t(in[i])) << 16;
        if (i + 1 < in.size()) {
            n |= std::uint32_t(std::uint8_t(in[i + 1])) << 8;
        }
        out += chars[(n >> 18) & 63];
        out += chars[(n >> 12) & 63];
        out += i + 1 < in.size() ? chars[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

} // namespace AfterglowVt::detail
