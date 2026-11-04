// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Sha256.h>

#include <algorithm>
#include <bit>
#include <cstring>

namespace AfterglowProto {

namespace {

constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

} // namespace

void Sha256::block(const std::uint8_t* data)
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = std::uint32_t(data[4 * i]) << 24 | std::uint32_t(data[4 * i + 1]) << 16
            | std::uint32_t(data[4 * i + 2]) << 8 | std::uint32_t(data[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = m_state;
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t t1 = h + s1 + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    const std::uint32_t added[8] = {a, b, c, d, e, f, g, h};
    for (int i = 0; i < 8; ++i) {
        m_state[std::size_t(i)] += added[i];
    }
}

void Sha256::update(std::string_view bytes)
{
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    std::size_t size = bytes.size();
    m_length += size;
    if (m_buffered) {
        const std::size_t take = std::min(size, m_buffer.size() - m_buffered);
        std::memcpy(m_buffer.data() + m_buffered, data, take);
        m_buffered += take;
        data += take;
        size -= take;
        if (m_buffered < m_buffer.size()) {
            return;
        }
        block(m_buffer.data());
        m_buffered = 0;
    }
    for (; size >= 64; data += 64, size -= 64) {
        block(data);
    }
    if (size) {
        std::memcpy(m_buffer.data(), data, size);
    }
    m_buffered = size;
}

Sha256::Digest Sha256::finish()
{
    const std::uint64_t bits = m_length * 8;
    static constexpr char pad[64] = {char(0x80)};
    update(std::string_view(pad, m_buffered < 56 ? 56 - m_buffered : 120 - m_buffered));
    char length[8];
    for (int i = 0; i < 8; ++i) {
        length[i] = char(bits >> (56 - 8 * i));
    }
    update(std::string_view(length, 8));
    Digest out;
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            out[4 * i + byte] = std::uint8_t(m_state[i] >> (24 - 8 * byte));
        }
    }
    *this = Sha256{};
    return out;
}

Sha256::Digest sha256(std::string_view bytes)
{
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}

Sha256::Digest hmacSha256(std::string_view key, std::string_view message)
{
    std::array<char, 64> block{};
    if (key.size() > block.size()) {
        const auto digest = sha256(key);
        std::memcpy(block.data(), digest.data(), digest.size());
    } else {
        std::memcpy(block.data(), key.data(), key.size());
    }
    std::array<char, 64> inner;
    std::array<char, 64> outer;
    for (std::size_t i = 0; i < block.size(); ++i) {
        inner[i] = char(block[i] ^ 0x36);
        outer[i] = char(block[i] ^ 0x5c);
    }
    Sha256 hash;
    hash.update(std::string_view(inner.data(), inner.size()));
    hash.update(message);
    const auto innerDigest = hash.finish();
    hash.update(std::string_view(outer.data(), outer.size()));
    hash.update(std::string_view(reinterpret_cast<const char*>(innerDigest.data()), innerDigest.size()));
    return hash.finish();
}

std::string toHex(std::string_view bytes)
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

std::string toHex(const Sha256::Digest& digest)
{
    return toHex(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

} // namespace AfterglowProto
