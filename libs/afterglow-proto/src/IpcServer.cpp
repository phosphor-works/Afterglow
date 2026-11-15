// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/IpcServer.h>
#include <AfterglowProto/Lock.h>
#include <AfterglowProto/Socket.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace AfterglowProto {

namespace {

// The limit of PhosphorIpc::parseRequest.
constexpr std::size_t MaxArgs = 4096;

void set(std::string* error, std::string text)
{
    if (error) *error = std::move(text);
}

std::string line(const Json::Value& value)
{
    std::string text = Json::write(value);
    text += '\n';
    return text;
}

sockaddr_un address(const std::string& path)
{
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, path.data(), path.size());
    return a;
}

/// An id as PhosphorIpc::parseIntegralJsonNumber reads one: a whole number
/// that an int64 holds, whose double is exact or rounded.
std::optional<std::int64_t> requestId(double d)
{
    constexpr double Int64Min = double(std::numeric_limits<std::int64_t>::min());
    constexpr double Int64MaxExclusive = 9223372036854775808.0;
    if (!std::isfinite(d) || d < Int64Min || d >= Int64MaxExclusive || std::trunc(d) != d) return std::nullopt;
    return std::int64_t(d);
}

std::string lockPath(const std::string& path)
{
    return path + ".lock";
}

/// Whether @p path names the file of @p device and @p inode.
bool names(const std::string& path, dev_t device, ino_t inode)
{
    struct stat current{};
    return ::lstat(path.c_str(), &current) == 0 && current.st_dev == device && current.st_ino == inode;
}

} // namespace

std::optional<IpcRequest> parseIpcRequest(std::string_view text, std::string* error, std::optional<std::int64_t>* id)
{
    std::string reason;
    auto parsed = Json::parse(text, &reason);
    if (!parsed) {
        set(error, "malformed JSON: " + reason);
        return std::nullopt;
    }
    if (!parsed->isObject()) {
        set(error, "request root is not a JSON object");
        return std::nullopt;
    }
    IpcRequest request;
    request.type = (*parsed)["type"].toString();
    if (request.type.empty()) {
        set(error, "missing or non-string 'type' field");
        return std::nullopt;
    }
    if (const Json::Value* value = parsed->find("id"); value && !value->isNull()) {
        if (!value->isNumber()) {
            set(error, "'id' must be a number if present");
            return std::nullopt;
        }
        request.id = requestId(value->toNumber());
        if (!request.id) {
            set(error, "'id' must be a finite integer within JSON-double-precision int64 range");
            return std::nullopt;
        }
        if (id) *id = request.id;
    }
    request.target = (*parsed)["target"].toString();
    request.fn = (*parsed)["fn"].toString();
    if (request.type == "call" && request.target.empty()) {
        set(error, "'call' requires a non-empty 'target'");
        return std::nullopt;
    }
    if (request.type == "call" && request.fn.empty()) {
        set(error, "'call' requires a non-empty 'fn'");
        return std::nullopt;
    }
    if (request.type == "subscribe") {
        request.signal = (*parsed)["signal"].toString();
        if (request.target.empty() || request.signal.empty()) {
            set(error, "'subscribe' requires a non-empty 'target' and 'signal'");
            return std::nullopt;
        }
    }
    if (const Json::Value* value = parsed->find("subscriptionId"); value && !value->isNull()) {
        if (!value->isNumber()) {
            set(error, "'subscriptionId' must be a number if present");
            return std::nullopt;
        }
        const auto subscription = requestId(value->toNumber());
        if (!subscription) {
            set(error, "'subscriptionId' must be a finite integer within JSON-double-precision int64 range");
            return std::nullopt;
        }
        request.subscriptionId = *subscription;
    }
    // Id 0 is the protocol's "absent".
    if (request.type == "unsubscribe" && request.subscriptionId == 0) {
        set(error, "'unsubscribe' requires a non-zero 'subscriptionId'");
        return std::nullopt;
    }
    if (const Json::Value* args = parsed->find("args"); args && !args->isNull()) {
        if (!args->isArray()) {
            set(error, "'args' must be an array if present");
            return std::nullopt;
        }
        if (args->toArray().size() > MaxArgs) {
            set(error, "'args' length " + std::to_string(args->toArray().size()) + " exceeds limit of " + std::to_string(MaxArgs));
            return std::nullopt;
        }
        request.args = args->toArray();
    }
    return request;
}

std::string ipcReply(std::int64_t id, Json::Value result)
{
    return line(Json::Object{{"type", "reply"}, {"id", id}, {"result", std::move(result)}});
}

std::string ipcEvent(std::int64_t subscriptionId, Json::Array args)
{
    return line(Json::Object{{"type", "event"}, {"subscriptionId", subscriptionId}, {"args", std::move(args)}});
}

std::string ipcError(std::optional<std::int64_t> id, std::string_view code, std::string_view message)
{
    // As PhosphorIpc::buildError: id 0 is the protocol's "absent".
    Json::Object error{{"type", "error"}};
    if (id && *id != 0) error.emplace_back("id", *id);
    error.emplace_back("code", code);
    error.emplace_back("message", message);
    return line(std::move(error));
}

IpcListener::~IpcListener()
{
    close();
}

IpcListener::Listen IpcListener::listen(const std::string& path, std::string* error)
{
    close();
    if (std::string reason = socketPathError(path); !reason.empty()) {
        set(error, std::move(reason));
        return Listen::Failed;
    }
    const std::string lock = lockPath(path);
    const int lockFd = lockFile(lock, error);
    if (lockFd == LockHeld) {
        set(error, "another server listens on " + path);
        return Listen::Held;
    }
    if (lockFd == LockFailed) return Listen::Failed;
    // The lock is ours, so a socket file there is one that its server left.
    const auto fail = [&](std::string reason, int fd = -1) {
        if (fd >= 0) ::close(fd);
        unlockFile(lock, lockFd);
        set(error, std::move(reason));
        return Listen::Failed;
    };
    struct stat existing{};
    if (::lstat(path.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) return fail(path + " exists and is not a socket");
        if (existing.st_uid != ::geteuid()) return fail(path + " is a socket of another user");
        ::unlink(path.c_str());
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return fail(std::string("cannot create a socket: ") + std::strerror(errno));
    const sockaddr_un a = address(path);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        return fail("cannot listen on " + path + ": " + std::strerror(errno), fd);
    }
    // From here the file is ours: a failure removes it.
    struct stat bound{};
    if (::chmod(path.c_str(), 0600) != 0 || ::lstat(path.c_str(), &bound) != 0 || ::listen(fd, 16) != 0) {
        const int failure = errno;
        ::unlink(path.c_str());
        return fail("cannot listen on " + path + ": " + std::strerror(failure), fd);
    }
    // Opened while descriptors are left, for accept() when none are.
    m_reserve = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    m_fd = fd;
    m_lock = lockFd;
    m_path = path;
    m_device = bound.st_dev;
    m_inode = bound.st_ino;
    return Listen::Listening;
}

void IpcListener::close()
{
    if (m_fd < 0) return;
    ::close(m_fd);
    m_fd = -1;
    // Someone may have removed the file, and another server bound the path
    // after it took a new lock file.
    if (names(m_path, m_device, m_inode)) ::unlink(m_path.c_str());
    unlockFile(lockPath(m_path), m_lock);
    m_lock = -1;
    if (m_reserve >= 0) ::close(m_reserve);
    m_reserve = -1;
    m_starved = false;
    m_path.clear();
    m_device = 0;
    m_inode = 0;
}

bool IpcListener::stillOurs() const
{
    return m_fd >= 0 && names(m_path, m_device, m_inode);
}

int IpcListener::accept()
{
    m_starved = false;
    // Lost when a descriptor that the last close freed was taken before it
    // opened again: while descriptors are left, it comes back.
    if (m_fd >= 0 && m_reserve < 0) m_reserve = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    while (m_fd >= 0) {
        const int fd = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EINTR) continue;
            // ECONNABORTED: the client left before the accept; others wait.
            if (errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE) {
                if (m_reserve < 0) {
                    m_starved = true;
                    return -1;
                }
                // The connection stays in the queue, and the listener
                // readable, until it is accepted: take it and close it.
                ::close(m_reserve);
                const int waiting = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
                const int failure = errno;
                if (waiting >= 0) ::close(waiting);
                m_reserve = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                if (waiting >= 0) continue;
                // The descriptor that the close freed is gone again:
                // EMFILE, another thread of this process took it; ENFILE,
                // another process took the place in the system's table.
                m_starved = failure == EMFILE || failure == ENFILE;
            }
            return -1;
        }
        ucred peer{};
        socklen_t size = sizeof peer;
        // The effective user, as the client's own check of the server.
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::geteuid()) {
            ::close(fd);
            continue;
        }
        return fd;
    }
    return -1;
}

IpcServerConnection::IpcServerConnection(int fd)
    : m_fd(fd)
{
}

IpcServerConnection::~IpcServerConnection()
{
    close();
}

void IpcServerConnection::close()
{
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
}

int IpcServerConnection::release(std::string* unsent)
{
    if (m_fd < 0 || m_in.size() > m_inHead) return -1;
    *unsent = m_out.substr(m_outOffset);
    m_out.clear();
    m_outOffset = 0;
    m_in.clear();
    m_inHead = m_scanned = 0;
    return std::exchange(m_fd, -1);
}

bool IpcServerConnection::fill()
{
    char buffer[65536];
    while (m_fd >= 0) {
        const ssize_t n = ::read(m_fd, buffer, sizeof buffer);
        if (n > 0) {
            m_in.append(buffer, std::size_t(n));
            // Enough for a line over the limit: the rest waits.
            if (m_in.size() - m_inHead > IpcConnection::MaxRequestBytes) return true;
            continue;
        }
        if (n == 0) {
            m_eof = true;
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        close();
        return false;
    }
    return false;
}

void IpcServerConnection::refuseLongLine()
{
    send(ipcError(std::nullopt, IpcErrorCode::MalformedRequest,
                  "request line exceeds " + std::to_string(IpcConnection::MaxRequestBytes) + " bytes; closing connection"));
    flush();
    close();
}

bool IpcServerConnection::nextLine(std::string& out)
{
    for (bool filled = false; m_fd >= 0; filled = true) {
        if (const auto end = m_in.find('\n', m_inHead + m_scanned); end != std::string::npos) {
            // As PhosphorIpc::IpcRouter: the limit counts the '\n'.
            if (end + 1 - m_inHead > IpcConnection::MaxRequestBytes) {
                refuseLongLine();
                return false;
            }
            std::string_view text = std::string_view(m_in).substr(m_inHead, end - m_inHead);
            m_inHead = end + 1;
            m_scanned = 0;
            while (text.ends_with('\r')) text.remove_suffix(1);
            out.assign(text);
            if (m_inHead == m_in.size()) {
                m_in.clear();
                m_inHead = 0;
            }
            return true;
        }
        m_scanned = m_in.size() - m_inHead;
        // With its '\n' still to come, the line is longer than this.
        if (m_scanned >= IpcConnection::MaxRequestBytes) {
            refuseLongLine();
            return false;
        }
        if (m_eof) {
            // The client ended its side; a line without its '\n' is not a
            // request. The answers that wait still go out: flush() closes.
            m_in.clear();
            m_inHead = m_scanned = 0;
            if (!wantsWrite()) close();
            return false;
        }
        if (filled) return false;
        m_in.erase(0, m_inHead);
        m_inHead = 0;
        if (!fill()) return false;
    }
    return false;
}

IpcServerConnection::Read IpcServerConnection::nextRequest(IpcRequest& request, std::string* line)
{
    std::string text;
    if (!nextLine(text)) return Read::None;
    if (text.empty()) return Read::Skipped;
    std::string error;
    std::optional<std::int64_t> id;
    auto parsed = parseIpcRequest(text, &error, &id);
    if (!parsed) {
        send(ipcError(id, IpcErrorCode::MalformedRequest, error));
        if (++m_malformed > MaxMalformedLines) {
            flush();
            close();
        }
        return Read::Skipped;
    }
    m_malformed = 0;
    request = std::move(*parsed);
    if (line) *line = std::move(text);
    return Read::Request;
}

bool IpcServerConnection::pending() const
{
    if (m_fd < 0) return false;
    return m_in.find('\n', m_inHead + m_scanned) != std::string::npos;
}

void IpcServerConnection::send(std::string text)
{
    if (m_fd < 0) return;
    if (m_out.size() - m_outOffset + text.size() > MaxWaitingBytes) {
        close();
        return;
    }
    if (m_outOffset == m_out.size()) {
        m_out = std::move(text);
        m_outOffset = 0;
    } else {
        // What went out is dropped once it is half the buffer: the buffer
        // stays below twice what waits.
        if (m_outOffset >= m_out.size() / 2) {
            m_out.erase(0, m_outOffset);
            m_outOffset = 0;
        }
        m_out += text;
    }
    flush();
}

bool IpcServerConnection::flush()
{
    while (m_fd >= 0 && m_outOffset < m_out.size()) {
        const ssize_t n = ::send(m_fd, m_out.data() + m_outOffset, m_out.size() - m_outOffset, MSG_NOSIGNAL);
        if (n > 0) {
            m_outOffset += std::size_t(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        close();
        return false;
    }
    if (m_outOffset == m_out.size()) {
        m_out.clear();
        m_outOffset = 0;
        // The client ended its side and has every answer.
        if (m_eof && !pending()) close();
    }
    return m_fd >= 0;
}

} // namespace AfterglowProto
