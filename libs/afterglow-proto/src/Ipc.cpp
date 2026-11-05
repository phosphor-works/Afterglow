// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Ipc.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace AfterglowProto {

namespace {

void set(IpcConnection::Error* error, std::string code, std::string message, bool timedOut = false)
{
    if (error) *error = {std::move(code), std::move(message), timedOut};
}

std::int64_t now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

IpcConnection::~IpcConnection()
{
    close();
}

void IpcConnection::close()
{
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
    m_buffer.clear();
}

bool IpcConnection::connect(const std::string& path, Error* error)
{
    close();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof address.sun_path) {
        set(error, {}, "the socket path is empty or too long");
        return false;
    }
    std::memcpy(address.sun_path, path.data(), path.size());
    m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_fd < 0) {
        set(error, {}, std::strerror(errno));
        return false;
    }
    int result;
    do {
        result = ::connect(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof address);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        set(error, {}, std::strerror(errno));
        close();
        return false;
    }
    return true;
}

std::optional<std::int64_t> IpcConnection::send(const std::string& target, const std::string& fn, Json::Array args,
                                                 Error* error)
{
    if (m_fd < 0) {
        set(error, {}, "not connected");
        return std::nullopt;
    }
    const std::int64_t id = ++m_nextId;
    std::string line = Json::write(Json::Object{{"type", "call"}, {"id", id}, {"target", target}, {"fn", fn},
                                                {"args", std::move(args)}});
    if (line.size() >= MaxRequestBytes) {
        set(error, RequestTooLong, "the request is over 1 MiB");
        return std::nullopt;
    }
    line += '\n';
    std::size_t written = 0;
    while (written < line.size()) {
        // MSG_NOSIGNAL: a server that went away is an error here, not SIGPIPE.
        const ssize_t n = ::send(m_fd, line.data() + written, line.size() - written, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            set(error, {}, std::string("the connection closed: ") + std::strerror(errno));
            close();
            return std::nullopt;
        }
        written += std::size_t(n);
    }
    return id;
}

std::optional<Json::Value> IpcConnection::call(const std::string& target, const std::string& fn, Json::Array args,
                                               int timeoutMs, Error* error, int wakeFd)
{
    const auto id = send(target, fn, std::move(args), error);
    if (!id) return std::nullopt;
    const std::int64_t deadline = now() + timeoutMs;
    while (true) {
        Json::Value frame;
        std::size_t bytes = 0;
        const Read status = readFrame(frame, int(std::max<std::int64_t>(0, deadline - now())), wakeFd, &bytes);
        if (status == Read::Timeout) {
            set(error, {}, "no reply in time", true);
            return std::nullopt;
        }
        if (status == Read::Woken) {
            if (error) *error = {{}, "interrupted", false, true};
            return std::nullopt;
        }
        if (status != Read::Frame) {
            set(error, {}, "the connection closed");
            return std::nullopt;
        }
        // The router cannot name the request of a line it could not read;
        // this client waits for one call at a time, so it is this one.
        if (answers(frame, *id) || (frame["type"].toString() == "error" && !frame.find("id"))) return answer(frame, error);
        m_waiting.emplace_back(std::move(frame), bytes);
    }
}

IpcConnection::Read IpcConnection::next(Json::Value& frame, int timeoutMs, int wakeFd, std::size_t* bytes)
{
    if (!m_waiting.empty()) {
        frame = std::move(m_waiting.front().first);
        if (bytes) *bytes = m_waiting.front().second;
        m_waiting.pop_front();
        return Read::Frame;
    }
    return readFrame(frame, timeoutMs, wakeFd, bytes);
}

bool IpcConnection::answers(const Json::Value& frame, std::int64_t id)
{
    const std::string& type = frame["type"].toString();
    return (type == "reply" || type == "error") && frame["id"].toInteger() == id;
}

std::optional<Json::Value> IpcConnection::answer(const Json::Value& frame, Error* error)
{
    if (frame["type"].toString() == "reply") return frame["result"];
    set(error, frame["code"].toString(), frame["message"].toString());
    return std::nullopt;
}

IpcConnection::Read IpcConnection::readFrame(Json::Value& frame, int timeoutMs, int wakeFd, std::size_t* bytes)
{
    const std::int64_t deadline = timeoutMs < 0 ? -1 : now() + timeoutMs;
    while (true) {
        if (const auto end = m_buffer.find('\n'); end != std::string::npos) {
            const std::string line = m_buffer.substr(0, end);
            m_buffer.erase(0, end + 1);
            if (bytes) *bytes = line.size() + 1;
            if (line.empty()) continue;
            auto parsed = Json::parse(line);
            if (!parsed || !parsed->isObject()) {
                close();
                return Read::Closed;
            }
            frame = std::move(*parsed);
            return Read::Frame;
        }
        if (m_fd < 0) return Read::Closed;
        if (m_buffer.size() > MaxFrameBytes) {
            close();
            return Read::Closed;
        }
        pollfd fds[2] = {{m_fd, POLLIN, 0}, {wakeFd, POLLIN, 0}};
        const int wait = deadline < 0 ? -1 : int(std::max<std::int64_t>(0, deadline - now()));
        const int ready = ::poll(fds, wakeFd >= 0 ? 2 : 1, wait);
        if (ready < 0 && errno == EINTR) {
            // A signal: its handler wrote to wakeFd, which the next poll sees.
            continue;
        }
        if (ready < 0) {
            close();
            return Read::Closed;
        }
        if (wakeFd >= 0 && (fds[1].revents & POLLIN)) return Read::Woken;
        if (ready == 0) return Read::Timeout;
        char chunk[65536];
        const ssize_t n = ::recv(m_fd, chunk, sizeof chunk, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) {
            close();
            return Read::Closed;
        }
        m_buffer.append(chunk, std::size_t(n));
    }
}

} // namespace AfterglowProto
