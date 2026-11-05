// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Ipc.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace AfterglowProto {

namespace {

void set(IpcConnection::Error* error, std::string code, std::string message, bool timedOut = false)
{
    if (error) *error = {std::move(code), std::move(message), timedOut};
}

void interrupted(IpcConnection::Error* error)
{
    if (error) *error = {{}, "interrupted", false, true};
}

std::int64_t now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t deadlineOf(int timeoutMs)
{
    return timeoutMs < 0 ? -1 : now() + timeoutMs;
}

/// The poll timeout until @p deadline: -1 without one.
int remaining(std::int64_t deadline)
{
    return deadline < 0 ? -1 : int(std::max<std::int64_t>(0, deadline - now()));
}

bool sendTimeout(int fd, int timeoutMs)
{
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    return ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
}

} // namespace

IpcConnection::~IpcConnection()
{
    close();
}

void IpcConnection::disconnect(std::string reason)
{
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
    m_buffer.clear();
    m_head = m_scanned = 0;
    m_closeReason = std::move(reason);
}

void IpcConnection::close()
{
    disconnect({});
    m_waiting.clear();
    m_waitingBytes = 0;
}

bool IpcConnection::connect(const std::string& path, Error* error, int timeoutMs)
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
    // A Unix socket connects at once, or waits while the server's queue is
    // full, up to SO_SNDTIMEO (0 there would wait without end). A signal
    // ends that wait with nothing connected, so connect() starts again: the
    // EALREADY of a TCP connect that goes on in the background never comes.
    if (timeoutMs >= 0 && !sendTimeout(m_fd, std::max(timeoutMs, 1))) {
        set(error, {}, std::strerror(errno));
        close();
        return false;
    }
    int result;
    do {
        result = ::connect(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof address);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        const bool late = errno == EAGAIN || errno == EWOULDBLOCK;
        set(error, {}, late ? "the server accepts no connection" : std::strerror(errno), late);
        close();
        return false;
    }
    // The sends have their own waits.
    if (timeoutMs >= 0 && !sendTimeout(m_fd, 0)) {
        set(error, {}, std::strerror(errno));
        close();
        return false;
    }
    ucred peer{};
    socklen_t size = sizeof peer;
    if (::getsockopt(m_fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::geteuid()) {
        set(error, {}, "the socket belongs to another user");
        close();
        return false;
    }
    return true;
}

std::optional<std::int64_t> IpcConnection::send(const std::string& target, const std::string& fn, Json::Array args,
                                                 Error* error, int wakeFd)
{
    return sendCall(target, fn, std::move(args), -1, wakeFd, error);
}

std::optional<std::int64_t> IpcConnection::sendCall(const std::string& target, const std::string& fn, Json::Array args,
                                                     std::int64_t deadline, int wakeFd, Error* error)
{
    if (m_fd < 0) {
        set(error, {}, m_closeReason.empty() ? "not connected" : m_closeReason);
        return std::nullopt;
    }
    std::string line = Json::write(Json::Object{{"type", "call"}, {"id", m_nextId + 1}, {"target", target}, {"fn", fn},
                                                {"args", std::move(args)}});
    if (line.size() >= MaxRequestBytes) {
        set(error, RequestTooLong, "the request is over " + std::to_string(MaxRequestBytes >> 20) + " MiB");
        return std::nullopt;
    }
    line += '\n';
    // A wait that can end needs a send that does not block.
    const bool waits = deadline >= 0 || wakeFd >= 0;
    std::size_t written = 0;
    while (written < line.size()) {
        // MSG_NOSIGNAL: a server that went away is an error here, not SIGPIPE.
        const ssize_t n = ::send(m_fd, line.data() + written, line.size() - written, MSG_NOSIGNAL | (waits ? MSG_DONTWAIT : 0));
        if (n > 0) {
            written += std::size_t(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue; // a signal's handler wrote to wakeFd, which the poll sees
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waits) {
            pollfd fds[2] = {{m_fd, POLLOUT, 0}, {wakeFd, POLLIN, 0}};
            const int ready = ::poll(fds, wakeFd >= 0 ? 2 : 1, remaining(deadline));
            if (ready < 0 && errno == EINTR) continue;
            const bool woken = ready > 0 && wakeFd >= 0 && fds[1].revents;
            if (ready > 0 && !woken) continue;
            // The server reads a line whole or not at all: part of one would
            // join the next request.
            if (written > 0) disconnect("a request was cut off");
            if (woken) {
                interrupted(error);
            } else if (ready == 0) {
                set(error, {}, "the server did not read the request in time", true);
            } else {
                set(error, {}, std::strerror(errno));
            }
            return std::nullopt;
        }
        const std::string why = std::string("the connection closed: ") + (n < 0 ? std::strerror(errno) : "nothing was written");
        set(error, {}, why);
        disconnect(why);
        return std::nullopt;
    }
    return ++m_nextId;
}

std::optional<Json::Value> IpcConnection::call(const std::string& target, const std::string& fn, Json::Array args,
                                               int timeoutMs, Error* error, int wakeFd)
{
    const std::int64_t deadline = deadlineOf(timeoutMs);
    const auto id = sendCall(target, fn, std::move(args), deadline, wakeFd, error);
    if (!id) return std::nullopt;
    while (true) {
        Json::Value frame;
        std::size_t bytes = 0;
        const Read status = readFrame(frame, deadline, wakeFd, &bytes);
        if (status == Read::Timeout) {
            set(error, {}, "no reply in time", true);
            return std::nullopt;
        }
        if (status == Read::Woken) {
            interrupted(error);
            return std::nullopt;
        }
        if (status != Read::Frame) {
            set(error, {}, m_closeReason);
            return std::nullopt;
        }
        // The router cannot name the request of a line it could not read;
        // this client waits for one call at a time, so it is this one.
        if (answers(frame, *id) || (frame["type"].toString() == "error" && !frame.find("id"))) {
            return answer(std::move(frame), error);
        }
        m_waitingBytes += bytes;
        m_waiting.emplace_back(std::move(frame), bytes);
        if (m_waitingBytes > MaxWaitingBytes) {
            disconnect("the server sent over " + std::to_string(MaxWaitingBytes >> 20) + " MiB before the answer");
            set(error, {}, m_closeReason);
            return std::nullopt;
        }
        // Frames that keep coming do not hold the call past its deadline.
        if (deadline >= 0 && now() >= deadline) {
            set(error, {}, "no reply in time", true);
            return std::nullopt;
        }
    }
}

IpcConnection::Read IpcConnection::next(Json::Value& frame, int timeoutMs, int wakeFd, std::size_t* bytes)
{
    if (!m_waiting.empty()) {
        frame = std::move(m_waiting.front().first);
        if (bytes) *bytes = m_waiting.front().second;
        m_waitingBytes -= m_waiting.front().second;
        m_waiting.pop_front();
        return Read::Frame;
    }
    return readFrame(frame, deadlineOf(timeoutMs), wakeFd, bytes);
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

std::optional<Json::Value> IpcConnection::answer(Json::Value&& frame, Error* error)
{
    if (frame["type"].toString() == "reply") {
        Json::Value* result = frame.find("result");
        return result ? std::move(*result) : Json::Value();
    }
    set(error, frame["code"].toString(), frame["message"].toString());
    return std::nullopt;
}

IpcConnection::Read IpcConnection::readFrame(Json::Value& frame, std::int64_t deadline, int wakeFd, std::size_t* bytes)
{
    while (true) {
        // Only the bytes after the ones that were searched before.
        if (const auto end = m_buffer.find('\n', m_head + m_scanned); end != std::string::npos) {
            const std::string_view line = std::string_view(m_buffer).substr(m_head, end - m_head);
            m_head = end + 1;
            m_scanned = 0;
            if (line.empty()) continue;
            std::string reason;
            auto parsed = Json::parse(line, &reason);
            if (!parsed || !parsed->isObject()) {
                disconnect("the server sent a line that is not a JSON object" + (reason.empty() ? "" : ": " + reason));
                return Read::Closed;
            }
            if (bytes) *bytes = line.size() + 1;
            frame = std::move(*parsed);
            return Read::Frame;
        }
        m_scanned = m_buffer.size() - m_head;
        if (m_fd < 0) return Read::Closed;
        if (m_scanned > MaxFrameBytes) {
            disconnect("the server sent a line over " + std::to_string(MaxFrameBytes >> 20) + " MiB");
            return Read::Closed;
        }
        // What is left is the start of a line: it moves to the front once,
        // and the frames before it are not copied again.
        m_buffer.erase(0, m_head);
        m_head = 0;
        pollfd fds[2] = {{m_fd, POLLIN, 0}, {wakeFd, POLLIN, 0}};
        const int ready = ::poll(fds, wakeFd >= 0 ? 2 : 1, remaining(deadline));
        if (ready < 0 && errno == EINTR) {
            // A signal: its handler wrote to wakeFd, which the next poll sees.
            continue;
        }
        if (ready < 0) {
            disconnect(std::strerror(errno));
            return Read::Closed;
        }
        // A wake descriptor that hung up or is not open ends the wait too:
        // a poll would never wait on it again.
        if (wakeFd >= 0 && fds[1].revents) return Read::Woken;
        if (ready == 0) return Read::Timeout;
        char chunk[65536];
        const ssize_t n = ::recv(m_fd, chunk, sizeof chunk, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) {
            disconnect(n == 0 ? "the server closed the connection" : std::strerror(errno));
            return Read::Closed;
        }
        m_buffer.append(chunk, std::size_t(n));
    }
}

} // namespace AfterglowProto
