// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Proxy.h"

#include "MuxConnect.h"
#include "MuxProtocol.h"
#include "MuxStart.h"
#include "ProcessFds.h"

#include <AfterglowProto/Socket.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

extern char** environ;

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

/// Makes @p fd non-blocking: its flags before, or -1 when it cannot.
int nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return flags;
}

/// Reads what @p d.from has, up to MaxWaiting. A read that fails ends the
/// direction as its end does: what waits still goes. A daemon that closed
/// with input unread gives ECONNRESET after its last bytes, for one.
void fill(Direction& d)
{
    char buffer[65536];
    while (d.waiting.size() < MaxWaiting) {
        const ssize_t n = ::read(d.from, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0) {
            d.ended = true;
            return;
        }
        d.waiting.append(buffer, std::size_t(n));
    }
}

/// Writes what waits to @p d.to. False on a failure, a closed reader too.
bool drain(Direction& d)
{
    while (!d.waiting.empty()) {
        const ssize_t n = ::write(d.to, d.waiting.data(), d.waiting.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK;
        d.waiting.erase(0, std::size_t(n));
    }
    return true;
}

/// The flags of standard input and output as the proxy found them: they
/// belong to descriptions that other processes may share (a shell that ran
/// it), and go back as they were.
struct Standard
{
    std::array<int, 2> flags{-1, -1};
    void restore(int fd)
    {
        if (flags[std::size_t(fd)] >= 0)
            ::fcntl(fd, F_SETFL, flags[std::size_t(fd)]);
        flags[std::size_t(fd)] = -1;
    }
    ~Standard()
    {
        restore(0);
        restore(1);
    }
};

/// Copies standard input to @p socket and @p socket to standard output until
/// both directions ended. The daemon's end of its output does not end the
/// input: a window's stream that the daemon finished still reads what the
/// window sends until then (Attachment::readLate). The daemon's close does.
int bridge(int socket)
{
    Standard standard;
    for (const int fd : {0, 1}) {
        standard.flags[std::size_t(fd)] = nonblocking(fd);
        if (standard.flags[std::size_t(fd)] < 0) {
            std::fprintf(stderr, "aglow-mux proxy: cannot make standard %s non-blocking: %s\n",
                         fd == 0 ? "input" : "output", std::strerror(errno));
            return 1;
        }
    }
    if (nonblocking(socket) < 0) {
        std::fprintf(stderr, "aglow-mux proxy: cannot make its connection non-blocking: %s\n", std::strerror(errno));
        return 1;
    }
    std::array<Direction, 2> directions{Direction{0, socket, {}}, Direction{socket, 1, {}}};
    Direction& input = directions[0];
    Direction& output = directions[1];
    bool inputEnded = false; // the end of standard input went to the daemon, or the daemon takes no more
    bool outputEnded = false; // the end of the daemon's output went to standard output
    while (!(inputEnded && outputEnded)) {
        std::array<pollfd, 5> fds{};
        nfds_t count = 0;
        // The reads of each direction while its buffer has room, and its
        // writes while it holds bytes.
        for (Direction& d : directions) {
            if (!d.ended && d.waiting.size() < MaxWaiting)
                fds[count++] = {d.from, POLLIN, 0};
            if (!d.waiting.empty())
                fds[count++] = {d.to, POLLOUT, 0};
        }
        // Once the output ended, the daemon's close shows as a hangup: what
        // the client sends then goes nowhere.
        const nfds_t hangup = count;
        if (outputEnded)
            fds[count++] = {socket, 0, 0};
        // Each iteration that does not end the loop waits for something.
        if (::poll(fds.data(), count, -1) < 0 && errno != EINTR) {
            std::fprintf(stderr, "aglow-mux proxy: poll failed: %s\n", std::strerror(errno));
            return 1;
        }
        if (outputEnded && (fds[hangup].revents & (POLLHUP | POLLERR))) {
            input.ended = true;
            input.waiting.clear();
            inputEnded = true;
        }
        if (!input.ended)
            fill(input);
        // A daemon that takes no more ends the input the same way.
        if (!inputEnded && !drain(input)) {
            input.ended = true;
            input.waiting.clear();
            inputEnded = true;
        }
        if (!output.ended)
            fill(output);
        // The client went: nothing reads what either end sends.
        if (!drain(output))
            return 0;
        if (!inputEnded && input.ended && input.waiting.empty()) {
            // Standard input ended and all of it went: the daemon hears the
            // end of the client's side.
            ::shutdown(socket, SHUT_WR);
            inputEnded = true;
        }
        if (!outputEnded && output.ended && output.waiting.empty()) {
            // The daemon ended its output, and all that it sent went: the
            // client hears the end. Standard output may be the socket of
            // standard input too (sshd can give one socket for both), which
            // a close alone would not end.
            standard.restore(1);
            // With one description for both, the restore made standard input
            // blocking too, and fill() would then wait in read() for the
            // client, deaf to the daemon's close. It stays non-blocking until
            // the end restores it (Standard).
            if (!inputEnded) {
                const int flags = ::fcntl(0, F_GETFL);
                if (flags >= 0 && !(flags & O_NONBLOCK))
                    ::fcntl(0, F_SETFL, flags | O_NONBLOCK);
            }
            ::shutdown(1, SHUT_WR);
            ::close(1);
            outputEnded = true;
        }
    }
    return 0;
}

/// The proxy's environment: that of the user's ssh login, which a session
/// that the client starts with callerEnvironment takes.
std::vector<std::pair<std::string, std::string>> ownEnvironment()
{
    std::vector<std::pair<std::string, std::string>> out;
    for (char** e = environ; e && *e; ++e) {
        const std::string_view entry(*e);
        const std::size_t eq = entry.find('=');
        if (eq != std::string_view::npos && eq > 0)
            out.emplace_back(std::string(entry.substr(0, eq)), std::string(entry.substr(eq + 1)));
    }
    return out;
}

/// The path of this executable, which a daemon that the proxy starts runs.
std::string ownPath()
{
#if defined(__FreeBSD__)
    return selfExecutable();
#else
    char path[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0)
        return {};
    return std::string(path, std::size_t(n));
#endif
}

} // namespace

int runProxy(bool start)
{
    // A client that went must not end the proxy with a signal: a write
    // fails, and it ends.
    std::signal(SIGPIPE, SIG_IGN);
    const AfterglowProto::MuxSockets sockets = AfterglowProto::muxSockets();
    if (sockets.candidates.empty()) {
        std::fprintf(stderr, "aglow-mux proxy: %s\n", sockets.error.c_str());
        return 1;
    }
    std::string error;
    Reached reached;
    if (start) {
        const std::string daemon = ownPath();
        if (daemon.empty()) {
            std::fprintf(stderr, "aglow-mux proxy: cannot find its own executable\n");
            return 1;
        }
        reached = connectOrStart(sockets, daemon, &error);
    } else {
        reached = connectAny(sockets, &error);
    }
    if (!reached.connection) {
        if (error.empty())
            return ProxyNoDaemon;
        std::fprintf(stderr, "aglow-mux proxy: %s\n", error.c_str());
        return 1;
    }
    // The daemon reads the environment of no other process: the proxy sends
    // its own first, for a newSession of the client with callerEnvironment.
    // Its answer goes no further. A daemon that refuses it (one that an
    // older build started, still running its sessions) is still carried:
    // only such a newSession fails, and says why.
    AfterglowProto::IpcConnection::Error environmentError;
    (void)reached.connection->call(Target, Fn::CallerEnvironment,
                                   AfterglowProto::Json::Array{ProtocolVersion, environmentJson(ownEnvironment())},
                                   CallTimeoutMs, &environmentError);
    AfterglowProto::IpcConnection::Error releaseError;
    const int socket = reached.connection->release(&releaseError);
    if (socket < 0) {
        std::fprintf(stderr, "aglow-mux proxy: %s\n", releaseError.message.c_str());
        return 1;
    }
    const int status = bridge(socket);
    ::close(socket);
    return status;
}

} // namespace AfterglowMux
