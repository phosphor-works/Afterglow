// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Daemon.h"

#include <AfterglowProto/Ipc.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;
using AfterglowProto::ipcError;
using AfterglowProto::ipcReply;
using AfterglowProto::IpcRequest;
namespace IpcErrorCode = AfterglowProto::IpcErrorCode;

namespace {

// The client that starts the daemon connects at once. A daemon that nobody
// reaches in this time ends, so a client that died before it connected
// leaves no process behind.
constexpr int StartWaitMs = 10000;

/// Creates @p directory and its parents with mode 0700, and checks that it
/// is ours and closed to others: the socket in it takes control of sessions.
bool privateDirectory(const std::string& directory, std::string* error)
{
    for (std::size_t slash = directory.find('/', 1);; slash = directory.find('/', slash + 1)) {
        const std::string part = directory.substr(0, slash);
        if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) {
            if (error) *error = "cannot create " + part + ": " + std::strerror(errno);
            return false;
        }
        if (slash == std::string::npos) break;
    }
    struct stat info{};
    if (::lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::getuid()
        || (info.st_mode & 0077) != 0) {
        if (error) *error = directory + " must be a directory of this user with mode 0700";
        return false;
    }
    return true;
}

} // namespace

Daemon::Daemon()
{
    m_wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    sigset_t signals;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGTERM);
    ::sigaddset(&signals, SIGINT);
    ::sigaddset(&signals, SIGHUP);
    // Blocked in this thread before any PTY thread starts, so the threads
    // inherit the mask and the signals arrive only through the signalfd.
    ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    m_signalFd = ::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    std::signal(SIGPIPE, SIG_IGN);
}

Daemon::~Daemon()
{
    // The programs end before the socket goes: a pane that runs
    // `afterglow ctl` finds no daemon only once its session ended.
    m_sessions.clear();
    m_connections.clear();
    m_listener.close();
    if (m_wakeFd >= 0) ::close(m_wakeFd);
    if (m_signalFd >= 0) ::close(m_signalFd);
}

bool Daemon::listen(const std::string& path, const std::string& directory, std::string* error)
{
    if (m_wakeFd < 0 || m_signalFd < 0) {
        if (error) *error = std::string("cannot create the event descriptors: ") + std::strerror(errno);
        return false;
    }
    if (!directory.empty() && !privateDirectory(directory, error)) return false;
    return m_listener.listen(path, error);
}

int Daemon::run()
{
    const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(StartWaitMs);
    while (true) {
        std::vector<pollfd> fds;
        fds.push_back({m_listener.fd(), POLLIN, 0});
        fds.push_back({m_wakeFd, POLLIN, 0});
        fds.push_back({m_signalFd, POLLIN, 0});
        for (const auto& connection : m_connections) {
            fds.push_back({connection->fd(), short(POLLIN | (connection->wantsWrite() ? POLLOUT : 0)), 0});
        }
        // Until the first session, the start wait bounds the poll: then an
        // idle daemon ends even when nothing wakes it.
        int timeout = -1;
        if (!m_hadSession) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(startDeadline - std::chrono::steady_clock::now());
            timeout = int(std::max<std::int64_t>(left.count(), 0)) + 1;
        }
        const int ready = ::poll(fds.data(), fds.size(), timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "afterglow-mux: poll failed: %s\n", std::strerror(errno));
            return 1;
        }
        if (fds[2].revents & POLLIN) {
            signalfd_siginfo info{};
            while (::read(m_signalFd, &info, sizeof info) == ssize_t(sizeof info)) { }
            return 0;
        }
        if (fds[1].revents & POLLIN) {
            std::uint64_t count = 0;
            (void)!::read(m_wakeFd, &count, sizeof count);
            reap();
        }
        for (std::size_t i = 3; i < fds.size(); ++i) {
            auto& connection = *m_connections[i - 3];
            if (fds[i].revents & POLLOUT) connection.flush();
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) read(connection);
        }
        std::erase_if(m_connections, [](const auto& c) { return !c->open(); });
        if (fds[0].revents & POLLIN) accept();
        if (idle() && (m_hadSession || std::chrono::steady_clock::now() >= startDeadline)) return 0;
    }
}

void Daemon::accept()
{
    for (int fd = m_listener.accept(); fd >= 0; fd = m_listener.accept()) {
        m_connections.push_back(std::make_unique<AfterglowProto::IpcServerConnection>(fd));
    }
}

void Daemon::read(AfterglowProto::IpcServerConnection& connection)
{
    std::string line;
    while (connection.open() && connection.nextLine(line)) {
        std::string error;
        std::optional<std::int64_t> id;
        const auto request = AfterglowProto::parseIpcRequest(line, &error, &id);
        if (!request) {
            connection.send(ipcError(id, IpcErrorCode::MalformedRequest, error));
            continue;
        }
        connection.send(answer(*request));
    }
}

std::string Daemon::answer(const IpcRequest& request)
{
    if (request.type != "call") {
        return ipcError(request.id, IpcErrorCode::MalformedRequest, "the mux daemon answers only calls, not '" + request.type + "'");
    }
    if (request.target != Target) {
        return ipcError(request.id, IpcErrorCode::NoSuchTarget, "no target '" + request.target + "'");
    }
    if (request.fn == Fn::Version) {
        return ipcReply(request.id.value_or(0), Json::Object{{"version", AFTERGLOW_VERSION}, {"protocol", ProtocolVersion}});
    }
    if (request.fn == Fn::NewSession) return newSession(request);
    if (request.fn == Fn::ListSessions) return listSessions(request);
    if (request.fn == Fn::KillSession) return killSession(request);
    return ipcError(request.id, IpcErrorCode::NoSuchFn, "no function '" + request.fn + "' on target '" + request.target + "'");
}

std::string Daemon::newSession(const IpcRequest& request)
{
    if (request.args.size() != 2) return ipcError(request.id, IpcErrorCode::InvalidArg, "newSession takes (protocol, options)");
    const auto protocol = request.args[0].toInteger();
    if (!protocol || *protocol != ProtocolVersion) {
        return ipcError(request.id, Error::VersionMismatch,
                        "the client speaks mux protocol " + (protocol ? std::to_string(*protocol) : std::string("?"))
                            + ", and the daemon speaks " + std::to_string(ProtocolVersion));
    }
    std::string error;
    auto options = sessionOptions(request.args[1], &error);
    if (!options) return ipcError(request.id, IpcErrorCode::InvalidArg, error);
    std::string name = options->name.empty() ? freeName() : options->name;
    if (find(name)) return ipcError(request.id, Error::SessionExists, "a session named '" + name + "' exists");
    auto session = std::make_unique<Session>(name, std::move(*options));
    const int wake = m_wakeFd;
    const auto exited = [wake] {
        const std::uint64_t one = 1;
        (void)!::write(wake, &one, sizeof one);
    };
    if (!session->addPane(m_nextPane++, m_listener.path(), exited, &error)) {
        return ipcError(request.id, Error::SpawnFailed, "cannot start the program of session '" + name + "': " + error);
    }
    m_sessions.push_back(std::move(session));
    m_hadSession = true;
    return ipcReply(request.id.value_or(0), Json::Object{{"name", name}});
}

std::string Daemon::listSessions(const IpcRequest& request)
{
    Json::Array sessions;
    for (const auto& session : m_sessions) sessions.push_back(toJson(session->info()));
    return ipcReply(request.id.value_or(0), std::move(sessions));
}

std::string Daemon::killSession(const IpcRequest& request)
{
    if (request.args.size() != 1 || !request.args[0].isString()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg, "killSession takes (name)");
    }
    const std::string& name = request.args[0].toString();
    const auto it = std::find_if(m_sessions.begin(), m_sessions.end(), [&](const auto& s) { return s->name() == name; });
    if (it == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    (*it)->kill();
    m_sessions.erase(it);
    return ipcReply(request.id.value_or(0), nullptr);
}

Session* Daemon::find(const std::string& name)
{
    for (const auto& session : m_sessions) {
        if (session->name() == name) return session.get();
    }
    return nullptr;
}

std::string Daemon::freeName() const
{
    for (std::uint64_t n = 1;; ++n) {
        std::string name = std::to_string(n);
        if (std::none_of(m_sessions.begin(), m_sessions.end(), [&](const auto& s) { return s->name() == name; })) return name;
    }
}

void Daemon::reap()
{
    std::erase_if(m_sessions, [](const auto& session) { return session->reap(); });
}

bool Daemon::idle() const
{
    return m_sessions.empty() && m_connections.empty();
}

} // namespace AfterglowMux
