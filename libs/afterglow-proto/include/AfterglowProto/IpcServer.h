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
// object on each line. aglow-mux has no Qt, and answers the clients that
// the Qt server of PhosphorIpc::IpcRouter answers too: `afterglow mux`, and
// in a session pane, whose AFTERGLOW_SOCKET names the daemon, `afterglow
// ctl`, `aglow run` and `afterglow ssh`.

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

/// Reads one request line with the rules of PhosphorIpc::parseRequest, in
/// its order, for the fields of a call. On a failure, @p error says why and
/// @p id gets the request's id when it has a valid one, so that the error
/// can name it.
std::optional<IpcRequest> parseIpcRequest(std::string_view line, std::string* error,
                                          std::optional<std::int64_t>* id = nullptr);

/// The lines that a server sends, each with its '\n'.
std::string ipcReply(std::int64_t id, Json::Value result);
std::string ipcError(std::optional<std::int64_t> id, std::string_view code, std::string_view message);

/// A listening Unix socket that takes connections only from processes of
/// its own user. It owns its path through an exclusive lock on PATH.lock,
/// which it holds while it listens: of the servers that try one path, one
/// gets it, and none removes the socket of another.
class IpcListener
{
public:
    enum class Listen {
        Listening,
        Held, ///< another server holds the lock of the path, and serves it
        Failed, ///< the error says why
    };

    IpcListener() = default;
    ~IpcListener();
    IpcListener(const IpcListener&) = delete;
    IpcListener& operator=(const IpcListener&) = delete;

    /// Takes the lock of @p path and binds it with mode 0600. A socket file
    /// that is there then is one that its server left when it ended: it is
    /// replaced. A path that is not a socket of this user is refused. The
    /// directory of @p path should be closed to other users: the mode of the
    /// socket file is set after bind(), and accept() refuses other users in
    /// any case.
    Listen listen(const std::string& path, std::string* error);
    /// Closes the socket, removes its file and its lock file, each only when
    /// it is still the one that this listener made, and lets go of the lock.
    void close();
    int fd() const { return m_fd; }
    const std::string& path() const { return m_path; }
    /// Whether the file at path() is still the socket that listen() bound.
    /// Not after someone removed it (logind removes the runtime directory
    /// at the last logout): a client then cannot reach this listener there.
    bool stillOurs() const;

    /// The next connection, non-blocking and close-on-exec, or -1 when none
    /// waits. A connection from another user is closed at once. When the
    /// process has no descriptor left (EMFILE, ENFILE), each waiting
    /// connection is accepted on a descriptor kept for that and closed: the
    /// listener does not stay readable, so a poll of it does not spin, and
    /// the client sees its connection close. A kept descriptor that was
    /// lost is opened again first.
    int accept();
    /// The last accept() left a connection waiting, as no descriptor was
    /// left and none was kept. The listener stays readable: a caller polls
    /// it again only after a descriptor was freed, or the poll spins.
    bool starved() const { return m_starved; }

private:
    int m_fd = -1;
    int m_lock = -1; ///< the descriptor of PATH.lock, which holds the lock
    int m_reserve = -1; ///< /dev/null, closed to make room for accept() at EMFILE
    bool m_starved = false;
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
    /// nothing must not grow the server. Replies count too: one reply that
    /// alone is longer closes the connection, so a server keeps its replies
    /// shorter.
    static constexpr std::size_t MaxWaitingBytes = 32 * 1024 * 1024;
    /// The malformed lines in a row that a client may send, as in
    /// PhosphorIpc::IpcRouter: the next one closes the connection, so that
    /// a client that sends only garbage gets no endless errors.
    static constexpr int MaxMalformedLines = 16;

    enum class Read {
        Request,
        Skipped, ///< an empty line, or one that got MALFORMED_REQUEST
        None, ///< no complete line: open() says whether the client is still there
    };

    explicit IpcServerConnection(int fd);
    ~IpcServerConnection();
    IpcServerConnection(const IpcServerConnection&) = delete;
    IpcServerConnection& operator=(const IpcServerConnection&) = delete;

    int fd() const { return m_fd; }
    bool open() const { return m_fd >= 0; }
    void close();
    /// Gives the descriptor up, for another protocol on the same connection
    /// (the stream of a mux attach), with the output that did not go yet in
    /// @p unsent. -1, and the connection stays, when the client sent more
    /// than the lines that the caller took: the next protocol would lose
    /// those bytes.
    int release(std::string* unsent);

    /// The next complete line in @p line, without its '\n' and the '\r'
    /// before it. Fills the buffer at most once (reads until the socket has
    /// nothing more, or a line over the limit is in), so that a client that
    /// sends without a pause cannot keep the caller here. An empty line is
    /// returned too: it is no request, but a caller that bounds its work for
    /// each connection counts it. False when no line is complete; then
    /// open() says whether the client is still there. A line over
    /// IpcConnection::MaxRequestBytes, its '\n' included, gets the error
    /// MALFORMED_REQUEST, and the connection closes.
    bool nextLine(std::string& line);
    /// The next line, read as a request into @p request. A line that is not
    /// one gets MALFORMED_REQUEST here.
    Read nextRequest(IpcRequest& request);
    /// A complete line waits: the next nextLine() returns it without a read.
    /// The caller then polls with a timeout of 0, as the socket may have
    /// nothing more to say.
    bool pending() const;
    /// Whether the client can send more. Not after it ended its side: its
    /// answers still go out, and the connection closes after them.
    bool wantsRead() const { return m_fd >= 0 && !m_eof; }
    /// Queues @p line and writes what the socket takes.
    void send(std::string line);
    /// Writes waiting output. False when the connection failed.
    bool flush();
    bool wantsWrite() const { return m_out.size() > m_outOffset; }

private:
    bool fill();
    void refuseLongLine();

    int m_fd = -1;
    std::string m_in;
    std::size_t m_inHead = 0;
    std::size_t m_scanned = 0; ///< bytes after m_inHead that have no '\n'
    bool m_eof = false;
    int m_malformed = 0; ///< malformed lines in a row
    std::string m_out;
    std::size_t m_outOffset = 0;
};

} // namespace AfterglowProto
