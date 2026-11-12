// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxConnect.h"

#include "MuxProtocol.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace AfterglowMux {

using AfterglowProto::IpcConnection;

namespace {

/// The version call on @p connection: empty when it speaks this protocol,
/// else why not. @p ended: the connection ended without an answer.
std::string checkVersion(IpcConnection& connection, int timeoutMs, bool* ended)
{
    IpcConnection::Error callError;
    const auto version = connection.call(AfterglowMux::Target, AfterglowMux::Fn::Version, {}, timeoutMs, &callError);
    if (!version) {
        *ended = callError.code.empty() && !callError.timedOut;
        return callError.message.empty() ? std::string("no answer") : callError.message;
    }
    const auto protocol = (*version)["protocol"].toInteger();
    if (protocol != AfterglowMux::ProtocolVersion) {
        return "afterglow-mux speaks protocol " + (protocol ? std::to_string(*protocol) : std::string("?")) + " and this afterglow speaks "
            + std::to_string(AfterglowMux::ProtocolVersion) + ": end its sessions with the afterglow that started it, or wait until they end";
    }
    return {};
}

} // namespace

ProxyProcess::~ProxyProcess()
{
    // Its connection closed before: it ends by itself. One that does not
    // goes.
    if (m_status == -2 && wait(1000) == -1 && m_status == -2) {
        ::kill(m_pid, SIGTERM);
        wait(1000);
        if (m_status == -2) {
            ::kill(m_pid, SIGKILL);
            while (::waitpid(m_pid, nullptr, 0) < 0 && errno == EINTR) { }
        }
    }
    if (m_errorFd >= 0) ::close(m_errorFd);
}

void ProxyProcess::readError()
{
    char buffer[4096];
    while (m_errorFd >= 0) {
        const ssize_t n = ::read(m_errorFd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                ::close(m_errorFd);
                m_errorFd = -1;
            }
            break;
        }
        // Enough for a message: a chatty command does not grow the client.
        if (m_error.size() < 4096) m_error.append(buffer, std::size_t(n));
    }
    while (m_error.ends_with('\n')) m_error.pop_back();
}

int ProxyProcess::wait(int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (m_status == -2) {
        readError();
        int status = 0;
        const pid_t reaped = ::waitpid(m_pid, &status, WNOHANG);
        if (reaped == m_pid) {
            readError();
            m_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        if (reaped < 0 && errno != EINTR) {
            m_status = -1;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return m_status;
}

Reached reach(const Route& route, bool start, std::string* error)
{
    error->clear();
    if (route.proxy.empty()) return connectAny(route.sockets, error);
    // Its standard input and output are one end of a socket pair, which the
    // connection takes; its standard error says why it failed.
    int pair[2] = {-1, -1};
    int errors[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0 || ::pipe2(errors, O_CLOEXEC) != 0) {
        *error = std::string("cannot make the pipes of the proxy: ") + std::strerror(errno);
        for (const int fd : {pair[0], pair[1], errors[0], errors[1]}) {
            if (fd >= 0) ::close(fd);
        }
        return {};
    }
    std::vector<std::string> words = route.proxy;
    words.emplace_back("proxy");
    if (start) words.emplace_back("--start");
    std::vector<char*> argv;
    for (auto& word : words) argv.push_back(word.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pair[1], 0);
    posix_spawn_file_actions_adddup2(&actions, pair[1], 1);
    posix_spawn_file_actions_adddup2(&actions, errors[1], 2);
    pid_t pid = -1;
    const int spawned = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pair[1]);
    ::close(errors[1]);
    if (spawned != 0) {
        ::close(pair[0]);
        ::close(errors[0]);
        *error = "cannot run " + words.front() + ": " + std::strerror(spawned);
        return {};
    }
    ::fcntl(errors[0], F_SETFL, ::fcntl(errors[0], F_GETFL) | O_NONBLOCK);
    auto proxy = std::make_shared<ProxyProcess>(pid, errors[0]);
    auto connection = std::make_unique<IpcConnection>();
    connection->adopt(pair[0]);
    bool ended = false;
    const std::string mismatch = checkVersion(*connection, ProxyCallTimeoutMs, &ended);
    if (mismatch.empty()) return {std::move(proxy), std::move(connection), {}};
    connection->close();
    const int status = proxy->wait(ended ? 5000 : 0);
    if (status == ProxyNoDaemon) return {};
    if (status == 127) {
        *error = route.host + " has no afterglow-mux (" + route.proxy.back() + "): install it there";
    } else if (ended && !proxy->errorOutput().empty()) {
        *error = proxy->errorOutput();
    } else {
        *error = mismatch;
    }
    return {};
}

Reached connectAny(const AfterglowProto::MuxSockets& sockets, std::string* error)
{
    error->clear();
    for (const AfterglowProto::MuxSocket& socket : sockets.candidates) {
        auto connection = std::make_unique<IpcConnection>();
        IpcConnection::Error connectError;
        if (!connection->connect(socket.path, &connectError, ConnectTimeoutMs)) {
            if (!connectError.noServer && error->empty()) *error = connectError.message;
            continue;
        }
        bool ended = false;
        if (const std::string failure = checkVersion(*connection, CallTimeoutMs, &ended); !failure.empty()) {
            // A daemon that closed the connection without an answer was
            // ending: as no daemon there.
            if (ended) continue;
            *error = failure;
            return {};
        }
        // A first socket that failed does not matter once one answers.
        error->clear();
        return {nullptr, std::move(connection), socket.path};
    }
    return {};
}

} // namespace AfterglowMux
