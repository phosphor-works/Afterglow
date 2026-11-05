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

    struct Error
    {
        std::string code; ///< the server's error code, empty when the connection failed
        std::string message;
        bool timedOut = false;
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
    /// frames that arrive meanwhile wait for next().
    std::optional<Json::Value> call(const std::string& target, const std::string& fn, Json::Array args, int timeoutMs,
                                    Error* error);
    /// The next frame, the ones that waited first. @p timeoutMs -1 waits
    /// without end. @p wakeFd, when not -1, ends the wait when it becomes
    /// readable (a signal's self-pipe).
    Read next(Json::Value& frame, int timeoutMs, int wakeFd = -1);

    /// A reply or an error frame for @p id: its result, or nothing with
    /// @p error set from the frame.
    static std::optional<Json::Value> answer(const Json::Value& frame, Error* error);
    static bool answers(const Json::Value& frame, std::int64_t id);

private:
    Read readFrame(Json::Value& frame, int timeoutMs, int wakeFd);

    int m_fd = -1;
    std::int64_t m_nextId = 0;
    std::string m_buffer;
    std::deque<Json::Value> m_waiting;
};

} // namespace AfterglowProto
