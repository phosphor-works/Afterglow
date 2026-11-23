// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Fd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#include <poll.h>
#include <signal.h>
#include <unistd.h>

/// What a process that decodes an untrusted image writes to its parent, and
/// how the parent reads it: aglow's decoder child (tools/aglow/Image.cpp) and
/// the decoding processes of aglow-mux (mux/src/ImageServer.cpp, read by
/// mux/src/ImageDecoder.cpp). A change of the record is a new
/// DecodeWireVersion of aglow-mux (mux/include/ImageDecoder.h): a window can
/// run an aglow-mux of another version.
namespace AfterglowProto {

/// One record on the pipe: this header, then length bytes.
struct DecodeRecord
{
    enum class Kind : std::uint32_t {
        Frame, ///< width x height RGBA8 pixels with straight alpha, shown for delay ms
        Warning, ///< text, at most MaxText bytes
        Error, ///< text, at most MaxText bytes: the decode failed, and nothing follows
        TooMany, ///< more frames than the parent takes: nothing follows
        End, ///< the decode finished
    };
    /// A Warning or an Error is never longer than this.
    static constexpr std::uint64_t MaxText = 4096;

    Kind kind = Kind::End;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t delay = 0;
    std::uint64_t length = 0;
};
// The bytes of the struct are the record: no padding to leak or to differ.
static_assert(std::is_trivially_copyable_v<DecodeRecord> && sizeof(DecodeRecord) == 24, "a record has no padding");

/// Writes @p record and @p payload to @p fd, which blocks. False when the
/// parent stopped reading.
inline bool writeDecodeRecord(int fd, const DecodeRecord& record, std::string_view payload = {})
{
    return writeAll(fd, std::string_view(reinterpret_cast<const char*>(&record), sizeof record))
        && writeAll(fd, payload);
}

/// Writes a Frame record of @p width x @p height pixels, and @p pixels.
inline bool writeDecodeFrame(int fd, std::uint32_t width, std::uint32_t height, std::int32_t delay, const void* pixels)
{
    const std::uint64_t size = std::uint64_t(width) * height * 4;
    const DecodeRecord record{DecodeRecord::Kind::Frame, width, height, delay, size};
    return writeDecodeRecord(fd, record, std::string_view(static_cast<const char*>(pixels), std::size_t(size)));
}

/// How a read from a decoding process ended.
enum class DecodeRead : std::uint8_t {
    Done,
    Ended, ///< the process closed its end, or the read failed
    Late, ///< DecodeWait::deadline passed
    Cancelled, ///< DecodeWait::cancel is readable
    Interrupted, ///< DecodeWait::stopped said so
};

/// What ends a wait for a decoding process besides its data. Each is
/// optional.
struct DecodeWait
{
    std::optional<std::chrono::steady_clock::time_point> deadline;
    /// A descriptor that ends the wait once it is readable, or -1.
    int cancel = -1;
    /// The signal mask while the wait polls (ppoll), for a caller that blocks
    /// the signals that stop it otherwise: one that arrives at any time ends
    /// the poll, where a read() that SA_RESTART restarts would wait on.
    const sigset_t* mask = nullptr;
    /// Asked before each poll: true when a caught signal stops the caller.
    bool (*stopped)() = nullptr;
};

/// Reads @p size bytes from the decoding process on @p fd.
inline DecodeRead readFromDecoder(int fd, void* data, std::size_t size, const DecodeWait& wait)
{
    auto* p = static_cast<char*>(data);
    while (size > 0) {
        if (wait.stopped && wait.stopped())
            return DecodeRead::Interrupted;
        timespec timeout{};
        const timespec* until = nullptr;
        if (wait.deadline) {
            const auto left =
                std::chrono::ceil<std::chrono::milliseconds>(*wait.deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
                return DecodeRead::Late;
            timeout.tv_sec = time_t(left.count() / 1000);
            timeout.tv_nsec = long(left.count() % 1000) * 1000000;
            until = &timeout;
        }
        pollfd ready[2] = {{fd, POLLIN, 0}, {wait.cancel, POLLIN, 0}};
        const int polled = ::ppoll(ready, wait.cancel >= 0 ? 2 : 1, until, wait.mask);
        if (polled < 0 && errno == EINTR)
            continue;
        if (polled < 0)
            return DecodeRead::Ended;
        if (polled == 0)
            return DecodeRead::Late;
        if (wait.cancel >= 0 && ready[1].revents)
            return DecodeRead::Cancelled;
        const ssize_t n = ::read(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return DecodeRead::Ended;
        p += n;
        size -= std::size_t(n);
    }
    return DecodeRead::Done;
}

/// Reads the header of the next record. Its length bytes are the caller's
/// to read, with readFromDecoder().
inline DecodeRead readDecodeRecord(int fd, DecodeRecord& record, const DecodeWait& wait)
{
    return readFromDecoder(fd, &record, sizeof record, wait);
}

} // namespace AfterglowProto
