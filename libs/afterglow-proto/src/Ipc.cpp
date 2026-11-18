// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Socket.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace AfterglowProto {

namespace {

void set(IpcConnection::Error* error, std::string code, std::string message, bool timedOut = false)
{
    if (error)
        *error = {std::move(code), std::move(message), timedOut};
}

void interrupted(IpcConnection::Error* error)
{
    if (error)
        *error = {{}, "interrupted", false, true};
}

std::int64_t now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
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

void IpcConnection::disconnect(std::string reason, bool notJson)
{
    if (m_fd >= 0)
        ::close(m_fd);
    m_fd = -1;
    m_buffer.clear();
    m_head = m_scanned = 0;
    m_closeReason = std::move(reason);
    m_closedOnText = notJson;
    m_sendClosed.clear();
}

void IpcConnection::setClosed(Error* error) const
{
    if (!error)
        return;
    set(error, {}, m_closeReason);
    error->notJson = m_closedOnText;
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
    if (std::string reason = socketPathError(path); !reason.empty()) {
        set(error, {}, std::move(reason));
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
    // Each start waits only what is left, so a signal that repeats does not
    // make the wait longer.
    const std::int64_t deadline = deadlineOf(timeoutMs);
    int result = -1;
    int failure = 0;
    for (bool first = true;; first = false) {
        if (deadline >= 0) {
            const int left = remaining(deadline);
            if (left == 0 && !first) {
                failure = EAGAIN;
                break;
            }
            if (!sendTimeout(m_fd, std::max(left, 1))) {
                set(error, {}, std::strerror(errno));
                close();
                return false;
            }
        }
        result = ::connect(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof address);
        if (result == 0)
            break;
        failure = errno;
        if (failure != EINTR)
            break;
    }
    if (result != 0) {
        const bool late = failure == EAGAIN || failure == EWOULDBLOCK;
        set(error, {}, late ? "the server accepts no connection" : std::strerror(failure), late);
        if (error)
            error->noServer = failure == ENOENT || failure == ECONNREFUSED;
        close();
        return false;
    }
    // The sends have their own waits.
    if (timeoutMs >= 0 && !sendTimeout(m_fd, 0)) {
        set(error, {}, std::strerror(errno));
        close();
        return false;
    }
    PeerCredentials peer;
    if (!peerCredentials(m_fd, peer) || peer.uid != ::geteuid()) {
        set(error, {}, "the socket belongs to another user");
        close();
        return false;
    }
    return true;
}

void IpcConnection::adopt(int fd)
{
    close();
    m_fd = fd;
}

int IpcConnection::release(Error* error)
{
    if (m_fd < 0) {
        set(error, {}, "not connected");
        return -1;
    }
    if (!m_waiting.empty() || m_buffer.size() > m_head) {
        set(error, {}, "the server sent more than its answer before the next protocol");
        close();
        return -1;
    }
    const int fd = std::exchange(m_fd, -1);
    close();
    return fd;
}

std::string IpcConnection::requestLine(std::int64_t id, const std::string& target, const std::string& fn,
                                       Json::Array args)
{
    return Json::write(
        Json::Object{{"type", "call"}, {"id", id}, {"target", target}, {"fn", fn}, {"args", std::move(args)}});
}

bool IpcConnection::fits(const std::string& target, const std::string& fn, const Json::Array& args)
{
    // The longest id: the largest integer that a JSON number holds exactly.
    return requestLine(std::int64_t(Json::MaxExactInteger) - 1, target, fn, args).size() < MaxRequestBytes;
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
        setClosed(error);
        if (error && error->message.empty())
            error->message = "not connected";
        return std::nullopt;
    }
    if (!m_sendClosed.empty()) {
        set(error, {}, m_sendClosed);
        return std::nullopt;
    }
    std::string line = requestLine(m_nextId + 1, target, fn, std::move(args));
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
        const ssize_t n =
            ::send(m_fd, line.data() + written, line.size() - written, MSG_NOSIGNAL | (waits ? MSG_DONTWAIT : 0));
        if (n > 0) {
            written += std::size_t(n);
            continue;
        }
        // A blocking send, without a wait to end, that a signal stopped
        // before any byte went. A send with a wait does not block: the
        // poll below sees the wake.
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waits) {
            pollfd fds[2] = {{m_fd, POLLOUT, 0}, {wakeFd, POLLIN, 0}};
            const int ready = ::poll(fds, wakeFd >= 0 ? 2 : 1, remaining(deadline));
            if (ready < 0 && errno == EINTR)
                continue;
            const int failure = errno;
            const bool woken = ready > 0 && wakeFd >= 0 && fds[1].revents;
            if (ready > 0 && !woken)
                continue;
            // The server reads a line whole or not at all: part of one would
            // join the next request.
            if (written > 0)
                disconnect("a request was cut off");
            if (woken) {
                interrupted(error);
            } else if (ready == 0) {
                set(error, {}, "the server did not read the request in time", true);
            } else {
                set(error, {}, std::strerror(failure));
            }
            return std::nullopt;
        }
        const int failure = n < 0 ? errno : 0;
        const std::string why =
            std::string("the connection closed: ") + (n < 0 ? std::strerror(failure) : "nothing was written");
        set(error, {}, why);
        // The server closed its end. What it sent before is still in the
        // socket, such as an error that says why: the reads go on to that
        // end, and only the sends stop.
        if (failure == EPIPE || failure == ECONNRESET) {
            m_sendClosed = why;
        } else {
            disconnect(why);
        }
        return std::nullopt;
    }
    return ++m_nextId;
}

std::optional<Json::Value> IpcConnection::call(const std::string& target, const std::string& fn, Json::Array args,
                                               int timeoutMs, Error* error, int wakeFd)
{
    const std::int64_t deadline = deadlineOf(timeoutMs);
    const auto id = sendCall(target, fn, std::move(args), deadline, wakeFd, error);
    // A server that closed its end before the request went can have said
    // why first: the reads go on to that end, for an error without an id.
    // Without one, the error of the send stays.
    if (!id && m_sendClosed.empty())
        return std::nullopt;
    while (true) {
        Json::Value frame;
        std::size_t bytes = 0;
        const Read status = readFrame(frame, deadline, wakeFd, &bytes);
        if (status == Read::Woken) {
            interrupted(error);
            return std::nullopt;
        }
        if (!id && status != Read::Frame) {
            // The send failed as the server closed: a line that is not JSON,
            // read before the close, says more than the send's error.
            if (m_closedOnText)
                setClosed(error);
            return std::nullopt;
        }
        if (status == Read::Timeout) {
            set(error, {}, "no reply in time", true);
            return std::nullopt;
        }
        if (status != Read::Frame) {
            setClosed(error);
            return std::nullopt;
        }
        // This client waits for one call at a time, so an error that names
        // no request answers this one.
        if ((id && answers(frame, *id)) || connectionError(frame))
            return answer(std::move(frame), error);
        m_waitingBytes += bytes;
        m_waiting.emplace_back(std::move(frame), bytes);
        if (m_waitingBytes > MaxWaitingBytes) {
            disconnect("the server sent over " + std::to_string(MaxWaitingBytes >> 20) + " MiB before the answer");
            setClosed(error);
            return std::nullopt;
        }
        // Frames that keep coming do not hold the call past its deadline.
        if (deadline >= 0 && now() >= deadline) {
            if (id)
                set(error, {}, "no reply in time", true);
            return std::nullopt;
        }
    }
}

IpcConnection::Read IpcConnection::next(Json::Value& frame, int timeoutMs, int wakeFd, std::size_t* bytes)
{
    if (!m_waiting.empty()) {
        frame = std::move(m_waiting.front().first);
        if (bytes)
            *bytes = m_waiting.front().second;
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

bool IpcConnection::connectionError(const Json::Value& frame)
{
    return frame["type"].toString() == "error" && !frame.find("id");
}

std::optional<Json::Value> IpcConnection::answer(const Json::Value& frame, Error* error)
{
    if (frame["type"].toString() == "reply")
        return frame["result"];
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
            if (line.empty())
                continue;
            std::string reason;
            auto parsed = Json::parse(line, &reason);
            if (!parsed || !parsed->isObject()) {
                disconnect("the server sent a line that is not a JSON object" + (reason.empty() ? "" : ": " + reason),
                           true);
                return Read::Closed;
            }
            if (bytes)
                *bytes = line.size() + 1;
            frame = std::move(*parsed);
            return Read::Frame;
        }
        m_scanned = m_buffer.size() - m_head;
        if (m_fd < 0)
            return Read::Closed;
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
        if (wakeFd >= 0 && fds[1].revents)
            return Read::Woken;
        if (ready == 0)
            return Read::Timeout;
        char chunk[65536];
        const ssize_t n = ::recv(m_fd, chunk, sizeof chunk, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0) {
            disconnect(n == 0 ? "the server closed the connection" : std::strerror(errno));
            return Read::Closed;
        }
        m_buffer.append(chunk, std::size_t(n));
    }
}

} // namespace AfterglowProto
