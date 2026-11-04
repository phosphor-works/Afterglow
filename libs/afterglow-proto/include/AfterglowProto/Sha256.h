// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace AfterglowProto {

/// SHA-256 (FIPS 180-4), for the protocol values that kitty defines with it:
/// the file transfer password hash and the drag and drop machine ID.
class Sha256
{
public:
    using Digest = std::array<std::uint8_t, 32>;

    void update(std::string_view bytes);
    /// The digest. The object starts over afterwards.
    Digest finish();

private:
    void block(const std::uint8_t* data);

    std::array<std::uint32_t, 8> m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> m_buffer{};
    std::size_t m_buffered = 0;
    std::uint64_t m_length = 0;
};

Sha256::Digest sha256(std::string_view bytes);
/// HMAC (RFC 2104) with SHA-256.
Sha256::Digest hmacSha256(std::string_view key, std::string_view message);
/// Lowercase hexadecimal.
std::string toHex(std::string_view bytes);
std::string toHex(const Sha256::Digest& digest);

} // namespace AfterglowProto
