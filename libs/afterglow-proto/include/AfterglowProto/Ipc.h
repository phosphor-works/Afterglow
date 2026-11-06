// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Json.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

/// A client of the phosphor-ipc wire format, for programs with no Qt: one JSON
/// object per line over a Unix socket. A request is
/// {"type":"call","id":N,"target":T,"fn":F,"args":[...]}. The server answers
/// {"type":"reply","id":N,"result":R} or {"type":"error","id":N,"code":C,
/// "message":M}, perhaps long after other replies when the target deferred the
/// call. An event for this connection only is {"type":"event","target":T,
/// "signal":S,"args":[...]}, and one for a subscription carries
/// "subscriptionId" in place of target and signal.
///
/// A timeout of -1 waits without end. A wake descriptor, when not -1, ends a
/// wait when it becomes readable or hangs up (a signal's self-pipe).
namespace AfterglowProto {

class IpcConnection
{
public:
    /// The router closes a connection whose request line is longer, '\n'
    /// included.
    static constexpr std::size_t MaxRequestBytes = 1024 * 1024;
    /// A longer line from the server is refused, so a broken server cannot
    /// grow the buffer without bound. Above the router's 16 MiB output limit.
    static constexpr std::size_t MaxFrameBytes = 32 * 1024 * 1024;
    /// The lines of the frames that come while call() waits for its answer,
    /// which then wait for next(). A server that sends more before the answer
    /// is broken: the connection closes.
    static constexpr std::size_t MaxWaitingBytes = MaxFrameBytes;

    /// The code of a request that this client did not send: longer than
    /// MaxRequestBytes. Not one of the server's codes.
    static constexpr const char* RequestTooLong = "REQUEST_TOO_LONG";

    struct Error
    {
        std::string code; ///< the server's error code or RequestTooLong, empty when the connection failed
        std::string message;
        bool timedOut = false;
        bool interrupted = false; ///< the wake descriptor ended the wait
        /// connect() found no server: no socket at the path, or nobody
        /// listens there (ENOENT, ECONNREFUSED).
        bool noServer = false;
    };

    enum class Read {
        Frame,
        Timeout,
        Closed, ///< the server closed the connection, or it failed: closeReason() says why
        Woken, ///< the wake descriptor became readable
    };

    IpcConnection() = default;
    ~IpcConnection();
    IpcConnection(const IpcConnection&) = delete;
    IpcConnection& operator=(const IpcConnection&) = delete;

    /// Connects to the socket at @p path. The wait while the server's queue
    /// of new connections is full ends after @p timeoutMs. A socket of
    /// another user is refused: it could read the calls and answer anything.
    bool connect(const std::string& path, Error* error, int timeoutMs = -1);
    bool connected() const { return m_fd >= 0; }
    /// The socket, for a poll of the caller's own: readable when a frame
    /// comes or the server closes the connection. -1 when not connected.
    int fd() const { return m_fd; }
    /// Closes the connection and drops the frames that wait for next().
    void close();
    /// Why the connection closed by itself. Empty while connected, and after
    /// close() or connect().
    const std::string& closeReason() const { return m_closeReason; }

    /// Whether a call of @p fn with @p args fits in one request, whatever
    /// its id: a caller can check that before the calls that must come first.
    static bool fits(const std::string& target, const std::string& fn, const Json::Array& args);
    /// Sends a call and returns its id, without waiting for the answer. A
    /// send blocks while the server reads nothing: @p wakeFd ends that wait
    /// (Error::interrupted). The connection closes when part of the request
    /// went out.
    std::optional<std::int64_t> send(const std::string& target, const std::string& fn, Json::Array args, Error* error,
                                     int wakeFd = -1);
    /// Sends a call and waits at most @p timeoutMs for its answer, the send
    /// included. Other frames that arrive meanwhile wait for next(). @p wakeFd
    /// ends the wait (Error::interrupted).
    std::optional<Json::Value> call(const std::string& target, const std::string& fn, Json::Array args, int timeoutMs,
                                    Error* error, int wakeFd = -1);
    /// The next frame, the ones that waited first. @p bytes gets the size of
    /// the frame's line, '\n' included.
    Read next(Json::Value& frame, int timeoutMs, int wakeFd = -1, std::size_t* bytes = nullptr);

    /// A reply or an error frame for @p id: its result, or nothing with
    /// @p error set from the frame.
    static std::optional<Json::Value> answer(const Json::Value& frame, Error* error);
    static std::optional<Json::Value> answer(Json::Value&& frame, Error* error);
    static bool answers(const Json::Value& frame, std::int64_t id);

private:
    static std::string requestLine(std::int64_t id, const std::string& target, const std::string& fn, Json::Array args);
    std::optional<std::int64_t> sendCall(const std::string& target, const std::string& fn, Json::Array args,
                                         std::int64_t deadline, int wakeFd, Error* error);
    Read readFrame(Json::Value& frame, std::int64_t deadline, int wakeFd, std::size_t* bytes);
    /// Closes the descriptor and keeps the frames that wait for next().
    void disconnect(std::string reason);

    int m_fd = -1;
    std::int64_t m_nextId = 0;
    std::string m_buffer;
    std::size_t m_head = 0; ///< where the bytes of m_buffer that no frame took start
    std::size_t m_scanned = 0; ///< how many of them have no '\n'
    std::deque<std::pair<Json::Value, std::size_t>> m_waiting; ///< frames and their line sizes
    std::size_t m_waitingBytes = 0;
    std::string m_closeReason;
};

} // namespace AfterglowProto
