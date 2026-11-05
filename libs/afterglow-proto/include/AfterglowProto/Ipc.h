// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Json.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

/// A client of the phosphor-ipc wire format, for programs with no Qt: one JSON
/// object per line over a Unix socket. A request is
/// {"type":"call","id":N,"target":T,"fn":F,"args":[...]}. The server answers
/// {"type":"reply","id":N,"result":R} or {"type":"error","id":N,"code":C,
/// "message":M}, perhaps long after other replies when the target deferred the
/// call. An event for this connection only is {"type":"event","target":T,
/// "signal":S,"args":[...]}, and one for a subscription carries
/// "subscriptionId" in place of target and signal.
namespace AfterglowProto {

class IpcConnection
{
public:
    /// The router closes a connection whose request line is longer.
    static constexpr std::size_t MaxRequestBytes = 1024 * 1024;
    /// A longer line from the server is refused, so a broken server cannot
    /// grow the buffer without bound. Above the router's 16 MiB output limit.
    static constexpr std::size_t MaxFrameBytes = 32 * 1024 * 1024;

    /// The code of a request that this client did not send: longer than
    /// MaxRequestBytes. Not one of the server's codes.
    static constexpr const char* RequestTooLong = "REQUEST_TOO_LONG";

    struct Error
    {
        std::string code; ///< the server's error code or RequestTooLong, empty when the connection failed
        std::string message;
        bool timedOut = false;
        bool interrupted = false; ///< the wake descriptor ended the wait
    };

    enum class Read {
        Frame,
        Timeout,
        Closed, ///< the server closed the connection, or it failed
        Woken, ///< the wake descriptor became readable
    };

    IpcConnection() = default;
    ~IpcConnection();
    IpcConnection(const IpcConnection&) = delete;
    IpcConnection& operator=(const IpcConnection&) = delete;

    bool connect(const std::string& path, Error* error);
    bool connected() const { return m_fd >= 0; }
    void close();

    /// Sends a call and returns its id, without waiting for the answer.
    std::optional<std::int64_t> send(const std::string& target, const std::string& fn, Json::Array args, Error* error);
    /// Sends a call and waits at most @p timeoutMs for its answer. Other
    /// frames that arrive meanwhile wait for next(). @p wakeFd, when not -1,
    /// ends the wait when it becomes readable (Error::interrupted).
    std::optional<Json::Value> call(const std::string& target, const std::string& fn, Json::Array args, int timeoutMs,
                                    Error* error, int wakeFd = -1);
    /// The next frame, the ones that waited first. @p timeoutMs -1 waits
    /// without end. @p wakeFd, when not -1, ends the wait when it becomes
    /// readable (a signal's self-pipe). @p bytes gets the size of the frame's
    /// line, '\n' included.
    Read next(Json::Value& frame, int timeoutMs, int wakeFd = -1, std::size_t* bytes = nullptr);

    /// A reply or an error frame for @p id: its result, or nothing with
    /// @p error set from the frame.
    static std::optional<Json::Value> answer(const Json::Value& frame, Error* error);
    static bool answers(const Json::Value& frame, std::int64_t id);

private:
    Read readFrame(Json::Value& frame, int timeoutMs, int wakeFd, std::size_t* bytes);

    int m_fd = -1;
    std::int64_t m_nextId = 0;
    std::string m_buffer;
    std::deque<std::pair<Json::Value, std::size_t>> m_waiting; ///< frames and their line sizes
};

} // namespace AfterglowProto
