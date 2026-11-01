// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace AfterglowVt::detail {

/// How much a decoder forgives.
enum class Base64Check {
    Lenient, ///< skips padding and anything outside the alphabet
    PaddingOptional, ///< the alphabet only, trailing padding optional
    Strict, ///< RFC 4648: the alphabet, length a multiple of four, correct padding
};

/// Decodes base64 onto the end of @p out; false (with @p out as it was)
/// when @p check rejects the input.
inline bool base64DecodeAppend(std::string& out, std::string_view in, Base64Check check)
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
    if (check == Base64Check::Strict) {
        if (in.size() % 4 != 0) {
            return false;
        }
        // At most two padding characters, and only at the end.
        const std::size_t padding = in.ends_with("==") ? 2 : in.ends_with('=') ? 1 : 0;
        in.remove_suffix(padding);
    } else if (check == Base64Check::PaddingOptional) {
        while (!in.empty() && in.back() == '=') {
            in.remove_suffix(1);
        }
        if (in.size() % 4 == 1) {
            return false; // six bits cannot end a byte
        }
    }
    const std::size_t start = out.size();
    out.resize(start + in.size() / 4 * 3 + 3);
    auto* dst = reinterpret_cast<unsigned char*>(out.data() + start);
    const auto* src = reinterpret_cast<const unsigned char*>(in.data());
    const auto* end = src + in.size();
    // Four characters at a time while they are all in the alphabet.
    while (end - src >= 4) {
        const std::int8_t a = table[src[0]];
        const std::int8_t b = table[src[1]];
        const std::int8_t c = table[src[2]];
        const std::int8_t d = table[src[3]];
        if ((a | b | c | d) < 0) {
            break;
        }
        const std::uint32_t n = (std::uint32_t(a) << 18) | (std::uint32_t(b) << 12) | (std::uint32_t(c) << 6) | std::uint32_t(d);
        dst[0] = static_cast<unsigned char>(n >> 16);
        dst[1] = static_cast<unsigned char>(n >> 8);
        dst[2] = static_cast<unsigned char>(n);
        dst += 3;
        src += 4;
    }
    // The rest a character at a time: the tail, padding, stray bytes.
    std::uint32_t acc = 0;
    int bits = 0;
    for (; src < end; ++src) {
        const std::int8_t v = table[*src];
        if (v < 0) {
            if (check == Base64Check::Lenient) {
                if (*src == '=') {
                    // Padding ends a quantum: its leftover bits are not data,
                    // so a following segment starts on a byte boundary.
                    acc = 0;
                    bits = 0;
                }
                continue;
            }
            out.resize(start);
            return false;
        }
        acc = (acc << 6) | std::uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            // n input characters give at most 3n/4 bytes, within the size
            // reserved above: skipped characters only ever shorten the output.
            *dst++ = static_cast<unsigned char>((acc >> bits) & 0xFF);
        }
    }
    out.resize(std::size_t(reinterpret_cast<char*>(dst) - out.data()));
    return true;
}

/// Decodes base64; nullopt when @p check rejects the input.
inline std::optional<std::string> base64Decode(std::string_view in, Base64Check check)
{
    std::string out;
    if (!base64DecodeAppend(out, in, check)) {
        return std::nullopt;
    }
    return out;
}

/// Decodes base64, skipping padding and anything outside the alphabet.
inline std::string base64Decode(std::string_view in)
{
    return *base64Decode(in, Base64Check::Lenient);
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
