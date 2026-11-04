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
    /// or the input is replaced.
    void setInput(std::string_view bytes);
    std::size_t inputLeft() const;
    /// One zlib call that writes at most @p capacity bytes to @p out and
    /// sets @p produced. End: the stream is complete. @p finish (Deflate
    /// only) says that no more input follows.
    Result step(char* out, std::size_t capacity, std::size_t& produced, bool finish = false);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

/// Decompresses the whole stream @p in onto the end of @p out. False when
/// the stream is invalid or incomplete, or its output would pass @p limit.
bool zlibInflate(std::string_view in, std::string& out, std::size_t limit);
/// Compresses @p in into one zlib stream.
std::string zlibCompress(std::string_view in);

} // namespace AfterglowProto
