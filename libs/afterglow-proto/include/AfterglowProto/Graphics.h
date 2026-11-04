// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// Kitty's graphics protocol (APC G): "ESC _ G k=v,k=v ; base64 payload ST".
/// https://sw.kovidgoyal.net/kitty/graphics-protocol/
namespace AfterglowProto {

/// The base64 bytes in one chunk of a chunked transmission.
inline constexpr std::size_t GraphicsChunk = 4096;

/// The control keys of one command. A key that is absent has the default.
struct GraphicsCommand
{
    char action = 't'; // a
    char medium = 'd'; // t
    char del = 'a'; // d
    char compression = 0; // o
    std::uint32_t format = 32; // f
    std::uint32_t s = 0; // width, or animation state for a=a
    std::uint32_t v = 0; // height, or loop count for a=a
    std::uint32_t size = 0; // S
    std::uint32_t offset = 0; // O
    std::uint32_t id = 0; // i
    std::uint32_t number = 0; // I
    std::uint32_t placement = 0; // p
    int more = 0; // m
    int quiet = 0; // q
    int x = 0;
    int y = 0;
    std::uint32_t ux = 0; // x and y unsigned, for the id range of d=r
    std::uint32_t uy = 0;
    int w = 0;
    int h = 0;
    int X = 0;
    std::int64_t Y = 0; // 64-bit for a frame's background colour; Yi elsewhere
    int Yi = 0;
    int c = 0;
    int r = 0;
    int C = 0;
    std::int32_t z = 0;
    int U = 0;
    std::uint32_t P = 0;
    std::uint32_t Q = 0;
    int H = 0;
    int V = 0;
    // Which keys the command carried, to tell a new command from a chunk.
    bool hasAction = false;
    bool hasId = false;
    bool hasNumber = false;

    /// Reads the keys before the ';'. Unknown keys and bad values are skipped.
    static GraphicsCommand parse(std::string_view keys);
    /// The keys that differ from the defaults, and those the command carried.
    /// x and y are written from ux and uy when those hold a value that x and y cannot.
    std::string keys() const;

    bool operator==(const GraphicsCommand&) const = default;
};

/// The complete command with @p payload (raw bytes, base64-encoded here).
/// A payload over one chunk goes in chunks of GraphicsChunk base64 bytes:
/// the first carries the keys and m=1, the others only m (and q, when set),
/// and the last m=0.
std::string writeGraphics(const GraphicsCommand& command, std::string_view payload);

/// A terminal's answer to a command that names an image.
struct GraphicsReply
{
    std::uint32_t id = 0;
    std::uint32_t number = 0;
    std::uint32_t placement = 0;
    std::string status; ///< "OK", or "ECODE:message"

    /// Reads the text between "ESC _ G" and the terminator.
    static std::optional<GraphicsReply> parse(std::string_view payload);
    /// The complete reply. i= only when the id is known, I= and p= when set.
    std::string write() const;

    bool ok() const { return status == "OK"; }
    bool operator==(const GraphicsReply&) const = default;
};

/// Decompresses o=z data, which may not grow past @p limit bytes.
bool graphicsInflate(std::string_view compressed, std::string& out, std::size_t limit);

} // namespace AfterglowProto
