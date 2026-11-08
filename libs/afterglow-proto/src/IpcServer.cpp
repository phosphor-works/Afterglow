// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/IpcServer.h>
#include <AfterglowProto/Socket.h>

#include <cerrno>
#include <cstring>

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

/// Whether a server answers on @p path. A connection that the kernel
/// refuses means that the socket file is stale.
bool answers(const std::string& path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    const sockaddr_un a = address(path);
    const bool connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
    ::close(fd);
    return connected;
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
    if (const Json::Value* value = parsed->find("id"); value && !value->isNull()) {
        if (!value->isNumber()) {
            set(error, "'id' must be a number if present");
            return std::nullopt;
        }
        request.id = value->toInteger();
        if (!request.id) {
            set(error, "'id' must be a finite integer within JSON-double-precision int64 range");
            return std::nullopt;
        }
        if (id) *id = request.id;
    }
    request.type = (*parsed)["type"].toString();
    if (request.type.empty()) {
        set(error, "missing or non-string 'type' field");
        return std::nullopt;
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

std::string ipcError(std::optional<std::int64_t> id, std::string_view code, std::string_view message)
{
    // As PhosphorIpc::buildError: id 0 is the protocol's "absent".
    Json::Object error{{"type", "error"}};
    if (id && *id != 0) error.emplace_back("id", *id);
    error.emplace_back("code", code);
    error.emplace_back("message", message);
    return line(std::move(error));
}

std::string ipcEvent(std::string_view target, std::string_view signal, Json::Array args)
{
    return line(Json::Object{{"type", "event"}, {"target", target}, {"signal", signal}, {"args", std::move(args)}});
}

IpcListener::~IpcListener()
{
    close();
}

bool IpcListener::listen(const std::string& path, std::string* error)
{
    close();
    if (path.empty() || path.size() > MaxSocketPathBytes) {
        set(error, "the socket path is over " + std::to_string(MaxSocketPathBytes) + " bytes: " + path);
        return false;
    }
    struct stat existing{};
    if (::lstat(path.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            set(error, path + " exists and is not a socket");
            return false;
        }
        if (answers(path)) {
            set(error, "another server listens on " + path);
            return false;
        }
        // Stale: its server ended without removing it.
        ::unlink(path.c_str());
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        set(error, std::string("cannot create a socket: ") + std::strerror(errno));
        return false;
    }
    const sockaddr_un a = address(path);
    // The file is 0600 from the moment it exists: no other user can connect
    // between bind() and a chmod().
    const mode_t mask = ::umask(0177);
    const bool bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
    const int bindErrno = errno;
    ::umask(mask);
    if (!bound || ::listen(fd, 16) != 0) {
        set(error, "cannot listen on " + path + ": " + std::strerror(bound ? errno : bindErrno));
        ::close(fd);
        return false;
    }
    struct stat file{};
    if (::stat(path.c_str(), &file) == 0) {
        m_device = file.st_dev;
        m_inode = file.st_ino;
    }
    m_fd = fd;
    m_path = path;
    return true;
}

void IpcListener::close()
{
    if (m_fd < 0) return;
    ::close(m_fd);
    m_fd = -1;
    // A second server may have replaced the file after ours went stale.
    struct stat current{};
    if (::lstat(m_path.c_str(), &current) == 0 && current.st_dev == m_device && current.st_ino == m_inode) {
        ::unlink(m_path.c_str());
    }
    m_path.clear();
}

int IpcListener::accept()
{
    while (m_fd >= 0) {
        const int fd = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EINTR) continue;
            // ECONNABORTED: the client left before the accept; others wait.
            if (errno == ECONNABORTED) continue;
            return -1;
        }
        ucred peer{};
        socklen_t size = sizeof peer;
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::getuid()) {
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

bool IpcServerConnection::nextLine(std::string& out)
{
    while (true) {
        if (const auto end = m_in.find('\n', m_inHead + m_scanned); end != std::string::npos) {
            std::string_view text = std::string_view(m_in).substr(m_inHead, end - m_inHead);
            m_inHead = end + 1;
            m_scanned = 0;
            if (text.size() > IpcConnection::MaxRequestBytes) {
                send(ipcError(std::nullopt, IpcErrorCode::MalformedRequest,
                              "request line exceeds " + std::to_string(IpcConnection::MaxRequestBytes) + " bytes; closing connection"));
                flush();
                close();
                return false;
            }
            while (text.ends_with('\r')) text.remove_suffix(1);
            if (text.empty()) continue;
            out.assign(text);
            if (m_inHead == m_in.size()) {
                m_in.clear();
                m_inHead = 0;
            }
            return true;
        }
        m_scanned = m_in.size() - m_inHead;
        if (m_scanned > IpcConnection::MaxRequestBytes) {
            send(ipcError(std::nullopt, IpcErrorCode::MalformedRequest,
                          "request line exceeds " + std::to_string(IpcConnection::MaxRequestBytes) + " bytes; closing connection"));
            flush();
            close();
            return false;
        }
        if (m_eof) {
            // The client ended; a line without its '\n' is not a request.
            close();
            return false;
        }
        m_in.erase(0, m_inHead);
        m_inHead = 0;
        const std::size_t before = m_in.size();
        if (!fill()) return false;
        if (m_in.size() == before && !m_eof) return false;
    }
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
    }
    return m_fd >= 0;
}

} // namespace AfterglowProto
