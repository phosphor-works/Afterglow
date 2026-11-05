// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace AfterglowProto {

/// A zlib stream (RFC 1950) that compresses or decompresses in steps, so a
/// caller can bound each step's output and stop between them.
class ZlibStream
{
public:
    enum class Mode : std::uint8_t { Inflate, Deflate };
    enum class Result : std::uint8_t { Ok, End, Error };

    explicit ZlibStream(Mode mode);
    ~ZlibStream();
    ZlibStream(const ZlibStream&) = delete;
    ZlibStream& operator=(const ZlibStream&) = delete;

    /// False when zlib could not set up the stream (no memory).
    bool valid() const;
    /// The next input. The bytes must stay valid until inputLeft() is zero
    /// or the input is replaced. zlib counts in 32 bits: give less than
    /// 4 GiB at a time, or the size wraps.
    void setInput(std::string_view bytes);
    /// Input bytes not yet taken. After End (Inflate), these are the bytes
    /// that follow the stream.
    std::size_t inputLeft() const;
    /// One zlib call that writes at most @p capacity bytes (under 4 GiB) to
    /// @p out and sets @p produced. End: the stream is complete. Error: the
    /// data is invalid, or no progress was possible (no input left on an
    /// incomplete stream, or no room).
    /// @p finish (Deflate only) says that no more input follows: once given,
    /// give it on every later step until End.
    Result step(char* out, std::size_t capacity, std::size_t& produced, bool finish = false);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

/// Decompresses the whole stream @p in onto the end of @p out. False, with
/// @p out as it was, when the stream is invalid or incomplete, or the data
/// appended would pass @p limit. Bytes after the end of the stream are
/// ignored. @p in must be under 4 GiB.
bool zlibInflate(std::string_view in, std::string& out, std::size_t limit);
/// Compresses all of @p in into one zlib stream, for clients and tests that
/// send a whole buffer. Empty when zlib fails (no memory):
/// a stream, even of no input, is never empty.
std::string zlibCompress(std::string_view in);

} // namespace AfterglowProto
