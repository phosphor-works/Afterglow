// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Proxy.h"

#include "MuxConnect.h"
#include "MuxStart.h"

#include <AfterglowProto/Socket.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <string>

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace AfterglowMux {

namespace {

/// The bytes that wait for each side at most: the other side is not read
/// while its buffer is full, so a slow end slows the other.
constexpr std::size_t MaxWaiting = std::size_t(1) << 20;

/// One direction of the connection: bytes from @p from, which wait in
/// @p waiting for @p to.
struct Direction
{
    int from;
    int to;
    std::string waiting;
    bool ended = false; ///< @p from ended: what waits still goes, then @p to ends
};

void nonblocking(int fd)
{
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/// Reads what @p d.from has, up to MaxWaiting. False on a failure.
bool fill(Direction& d)
{
    char buffer[65536];
    while (d.waiting.size() < MaxWaiting) {
        const ssize_t n = ::read(d.from, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK;
        if (n == 0) {
            d.ended = true;
            return true;
        }
        d.waiting.append(buffer, std::size_t(n));
    }
    return true;
}

/// Writes what waits to @p d.to. False on a failure, a closed reader too.
bool drain(Direction& d)
{
    while (!d.waiting.empty()) {
        const ssize_t n = ::write(d.to, d.waiting.data(), d.waiting.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK;
        d.waiting.erase(0, std::size_t(n));
    }
    return true;
}

/// Copies standard input to @p socket and @p socket to standard output until
/// both directions ended.
int bridge(int socket)
{
    for (const int fd : {0, 1, socket}) nonblocking(fd);
    std::array<Direction, 2> directions{Direction{0, socket, {}}, Direction{socket, 1, {}}};
    bool outputEnded = false; // the end of the socket's direction went to standard output
    bool inputEnded = false; // the end of standard input went to the daemon
    while (!(inputEnded && outputEnded)) {
        std::array<pollfd, 4> fds{};
        nfds_t count = 0;
        // The reads of each direction while its buffer has room, and its
        // writes while it holds bytes.
        for (Direction& d : directions) {
            if (!d.ended && d.waiting.size() < MaxWaiting) fds[count++] = {d.from, POLLIN, 0};
            if (!d.waiting.empty()) fds[count++] = {d.to, POLLOUT, 0};
        }
        if (count == 0 && !inputEnded && directions[0].ended && directions[0].waiting.empty()) {
            // Standard input ended and all of it went: the daemon hears the
            // end of the client's side.
            ::shutdown(socket, SHUT_WR);
            inputEnded = true;
            continue;
        }
        if (count > 0 && ::poll(fds.data(), count, -1) < 0 && errno != EINTR) return 1;
        for (Direction& d : directions) {
            if (!d.ended && !fill(d)) return 1;
            if (!drain(d)) return 0;
        }
        if (!inputEnded && directions[0].ended && directions[0].waiting.empty()) {
            ::shutdown(socket, SHUT_WR);
            inputEnded = true;
        }
        // The daemon closed the connection, and all that it sent went: the
        // client hears the end.
        if (!outputEnded && directions[1].ended && directions[1].waiting.empty()) {
            ::close(1);
            outputEnded = true;
            // The client's side means nothing more: the daemon's end is gone.
            inputEnded = true;
        }
    }
    return 0;
}

/// The path of this executable, which a daemon that the proxy starts runs.
std::string ownPath()
{
    char path[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return {};
    return std::string(path, std::size_t(n));
}

} // namespace

int runProxy(bool start)
{
    // A client that went must not end the proxy with a signal: a write
    // fails, and it ends.
    std::signal(SIGPIPE, SIG_IGN);
    const AfterglowProto::MuxSockets sockets = AfterglowProto::muxSockets();
    if (sockets.candidates.empty()) {
        std::fprintf(stderr, "afterglow-mux proxy: %s\n", sockets.error.c_str());
        return 1;
    }
    std::string error;
    Reached reached;
    if (start) {
        const std::string daemon = ownPath();
        if (daemon.empty()) {
            std::fprintf(stderr, "afterglow-mux proxy: cannot find its own executable\n");
            return 1;
        }
        reached = connectOrStart(sockets, daemon, &error);
    } else {
        reached = connectAny(sockets, &error);
    }
    if (!reached.connection) {
        if (error.empty()) return ProxyNoDaemon;
        std::fprintf(stderr, "afterglow-mux proxy: %s\n", error.c_str());
        return 1;
    }
    AfterglowProto::IpcConnection::Error releaseError;
    const int socket = reached.connection->release(&releaseError);
    if (socket < 0) {
        std::fprintf(stderr, "afterglow-mux proxy: %s\n", releaseError.message.c_str());
        return 1;
    }
    const int status = bridge(socket);
    ::close(socket);
    return status;
}

} // namespace AfterglowMux
