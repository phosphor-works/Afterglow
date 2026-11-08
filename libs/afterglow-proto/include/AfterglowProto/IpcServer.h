// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Json.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <sys/types.h>

// The server side of the phosphor-ipc wire format, without Qt: one JSON
// object on each line. A server that has no Qt (afterglow-mux) answers the
// same clients as the Qt server of PhosphorIpc::IpcRouter, so `afterglow ctl`
// and `aglow run` reach both with one client (IpcConnection).

namespace AfterglowProto {

namespace IpcErrorCode {
// The codes of PhosphorIpc::IpcProtocol::ErrorCode, which clients compare.
inline constexpr const char* NoSuchTarget = "NO_SUCH_TARGET";
inline constexpr const char* NoSuchFn = "NO_SUCH_FN";
inline constexpr const char* InvalidArg = "INVALID_ARG";
inline constexpr const char* MalformedRequest = "MALFORMED_REQUEST";
inline constexpr const char* CallFailed = "CALL_FAILED";
} // namespace IpcErrorCode

struct IpcRequest
{
    std::string type; ///< "call" is the only type that a server of this library answers
    std::optional<std::int64_t> id;
    std::string target;
    std::string fn;
    Json::Array args;
};

/// Reads one request line with the rules of PhosphorIpc::parseRequest. On a
/// failure, @p error says why and @p id gets the request's id when it has a
/// valid one, so that the error can name it.
std::optional<IpcRequest> parseIpcRequest(std::string_view line, std::string* error,
                                          std::optional<std::int64_t>* id = nullptr);

/// The lines that a server sends, each with its '\n'.
std::string ipcReply(std::int64_t id, Json::Value result);
std::string ipcError(std::optional<std::int64_t> id, std::string_view code, std::string_view message);
std::string ipcEvent(std::string_view target, std::string_view signal, Json::Array args);

/// A listening Unix socket that takes connections only from processes of
/// its own user.
class IpcListener
{
public:
    IpcListener() = default;
    ~IpcListener();
    IpcListener(const IpcListener&) = delete;
    IpcListener& operator=(const IpcListener&) = delete;

    /// Binds @p path with mode 0600. A socket file that no server answers is
    /// stale and is replaced. A path that a server answers, or that is not a
    /// socket, is refused: @p error says why.
    bool listen(const std::string& path, std::string* error);
    /// Closes the socket and removes its file, when it is still ours.
    void close();
    int fd() const { return m_fd; }
    const std::string& path() const { return m_path; }

    /// The next connection, non-blocking and close-on-exec, or -1 when none
    /// waits. A connection from another user is closed at once.
    int accept();

private:
    int m_fd = -1;
    std::string m_path;
    dev_t m_device = 0; ///< of the socket file that we bound, to remove only ours
    ino_t m_inode = 0;
};

/// One connection to a server: its lines in, its answers out. Writes never
/// block: what the socket does not take waits until flush().
class IpcServerConnection
{
public:
    /// Waiting output past this closes the connection: a client that reads
    /// nothing must not grow the server.
    static constexpr std::size_t MaxWaitingBytes = 32 * 1024 * 1024;

    explicit IpcServerConnection(int fd);
    ~IpcServerConnection();
    IpcServerConnection(const IpcServerConnection&) = delete;
    IpcServerConnection& operator=(const IpcServerConnection&) = delete;

    int fd() const { return m_fd; }
    bool open() const { return m_fd >= 0; }
    void close();

    /// Reads what the socket has, and returns the next complete line in
    /// @p line. False when no line is complete; then open() says whether
    /// the client is still there. A line over IpcConnection::MaxRequestBytes
    /// gets the error REQUEST_TOO_LONG, and the connection closes.
    bool nextLine(std::string& line);
    /// Queues @p line and writes what the socket takes.
    void send(std::string line);
    /// Writes waiting output. False when the connection failed.
    bool flush();
    bool wantsWrite() const { return m_out.size() > m_outOffset; }

private:
    bool fill();

    int m_fd = -1;
    std::string m_in;
    std::size_t m_inHead = 0;
    std::size_t m_scanned = 0;
    bool m_eof = false;
    std::string m_out;
    std::size_t m_outOffset = 0;
};

} // namespace AfterglowProto
