// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Daemon.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Run.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;
using AfterglowProto::ipcError;
using AfterglowProto::ipcReply;
using AfterglowProto::IpcRequest;
using AfterglowProto::IpcServerConnection;
namespace IpcErrorCode = AfterglowProto::IpcErrorCode;

namespace {

// The lines of one connection that one pass of the loop answers, empty ones
// included: a client that pipelines calls cannot keep the others, the
// signals and the reaping of programs waiting.
constexpr int MaxLinesPerRead = 64;

// ControlSocket::Target of the application. `afterglow ctl` in a session
// pane calls it here, and `aglow run` calls AfterglowProto::Run::Target:
// AFTERGLOW_SOCKET of the pane names this daemon.
constexpr const char* ControlTarget = "control";

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
    if (::lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()
        || (info.st_mode & 0077) != 0) {
        if (error) *error = directory + " must be a directory of this user with mode 0700";
        return false;
    }
    return true;
}

} // namespace

Daemon::Daemon(long fileLimit)
    : m_fileLimit(fileLimit)
{
    m_wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wakeFd < 0) m_setupError = std::string("cannot create an eventfd: ") + std::strerror(errno);
    sigset_t signals;
    ::sigemptyset(&signals);
    for (const int signal : {SIGTERM, SIGINT, SIGHUP}) {
        // A blocked signal reaches the signalfd even when its action is
        // SIG_IGN: one that the daemon got as ignored (nohup) stays out.
        struct sigaction previous{};
        if (::sigaction(signal, nullptr, &previous) == 0 && previous.sa_handler == SIG_IGN) continue;
        ::sigaddset(&signals, signal);
    }
    // Blocked in this thread before any PTY thread starts, so the threads
    // inherit the mask and the signals arrive only through the signalfd.
    ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    m_signalFd = ::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (m_signalFd < 0 && m_setupError.empty()) m_setupError = std::string("cannot create a signalfd: ") + std::strerror(errno);
    std::signal(SIGPIPE, SIG_IGN);
}

Daemon::~Daemon()
{
    // Each pane closes its terminal, which hangs up its programs, and the
    // PTY's reaper kills those still there after a grace time; nothing waits
    // for them to exit. The listeners close after that: until then their
    // locks keep a second daemon from starting next to programs that did
    // not get their hangup yet.
    m_sessions.clear();
    m_connections.clear();
    m_listeners.clear();
    if (m_wakeFd >= 0) ::close(m_wakeFd);
    if (m_signalFd >= 0) ::close(m_signalFd);
}

Daemon::Listen Daemon::listen(const std::vector<AfterglowProto::MuxSocket>& sockets, std::string* error)
{
    if (!m_setupError.empty()) {
        if (error) *error = m_setupError;
        return Listen::Failed;
    }
    // Why each socket that is left out is: the error names them when none
    // is left, for a client that sees no stderr of the daemon.
    std::string reasons;
    const auto skip = [&reasons](const std::string& reason) {
        std::fprintf(stderr, "afterglow-mux: %s\n", reason.c_str());
        reasons += (reasons.empty() ? "" : " and ") + reason;
    };
    for (const AfterglowProto::MuxSocket& socket : sockets) {
        std::string reason;
        if (!privateDirectory(socket.directory, &reason)) {
            skip(reason);
            continue;
        }
        auto listener = std::make_unique<AfterglowProto::IpcListener>();
        switch (listener->listen(socket.path, &reason)) {
        case AfterglowProto::IpcListener::Listen::Listening:
            m_listeners.push_back(std::move(listener));
            break;
        case AfterglowProto::IpcListener::Listen::Held:
            // Its sessions are there: a client that reached this daemon on
            // another socket would not see them.
            if (error) *error = "another afterglow-mux listens on " + socket.path;
            m_listeners.clear();
            return Listen::Held;
        case AfterglowProto::IpcListener::Listen::Failed:
            skip(reason);
            break;
        }
    }
    if (m_listeners.empty()) {
        if (error) *error = "no socket to listen on: " + reasons;
        return Listen::Failed;
    }
    return Listen::Listening;
}

Daemon::Exit Daemon::run()
{
    const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(StartWaitMs);
    const std::size_t wakeIndex = m_listeners.size();
    const std::size_t signalIndex = wakeIndex + 1;
    const std::size_t firstConnection = signalIndex + 1;
    std::vector<pollfd> fds;
    while (true) {
        fds.clear();
        // A starved listener stays readable, and would wake every poll: it
        // waits for the next wake of another kind. A descriptor comes back
        // only when a connection or a session closes, and both wake the
        // poll (a hangup, the wake eventfd).
        for (const auto& listener : m_listeners) fds.push_back({listener->starved() ? -1 : listener->fd(), POLLIN, 0});
        fds.push_back({m_wakeFd, POLLIN, 0});
        fds.push_back({m_signalFd, POLLIN, 0});
        bool pending = false;
        for (const auto& connection : m_connections) {
            const short events = short((connection->wantsRead() ? POLLIN : 0) | (connection->wantsWrite() ? POLLOUT : 0));
            fds.push_back({connection->fd(), events, 0});
            pending = pending || connection->pending();
        }
        const std::size_t firstAttachment = fds.size();
        int connectWait = -1;
        for (const auto& [name, attachment] : m_attachments) {
            fds.push_back(attachment->pollFd());
            pending = pending || attachment->busy();
            if (const int left = attachment->connectTimeoutMs(); left >= 0 && (connectWait < 0 || left < connectWait)) {
                connectWait = left;
            }
        }
        // Lines that wait in a buffer get their turn at once. Until the
        // first session, the start wait bounds the poll: then an idle daemon
        // ends even when nothing wakes it. After the wait, only a connection
        // keeps it, and its close wakes the poll.
        int timeout = -1;
        if (pending) {
            timeout = 0;
        } else if (!m_hadSession) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(startDeadline - std::chrono::steady_clock::now());
            if (left.count() >= 0) timeout = int(left.count()) + 1;
        }
        // A window that does not connect to its attach socket in time loses it.
        if (connectWait >= 0 && (timeout < 0 || connectWait + 1 < timeout)) timeout = connectWait + 1;
        const int ready = ::poll(fds.data(), fds.size(), timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "afterglow-mux: poll failed: %s\n", std::strerror(errno));
            return {1, 0};
        }
        if (fds[signalIndex].revents & POLLIN) {
            signalfd_siginfo info{};
            int signal = 0;
            while (::read(m_signalFd, &info, sizeof info) == ssize_t(sizeof info)) {
                if (signal == 0) signal = int(info.ssi_signo);
            }
            if (signal != 0) return {128 + signal, signal};
        }
        if (fds[wakeIndex].revents & POLLIN) {
            std::uint64_t count = 0;
            (void)!::read(m_wakeFd, &count, sizeof count);
            reap();
        }
        // The attachments in the order of the poll: a call below can add or
        // remove one, so their names are taken first.
        std::vector<std::pair<std::string, short>> attached;
        {
            std::size_t i = firstAttachment;
            for (const auto& [name, attachment] : m_attachments) attached.emplace_back(name, fds[i++].revents);
        }
        for (const auto& [name, revents] : attached) {
            const auto it = m_attachments.find(name);
            const auto session = find(name);
            if (it == m_attachments.end() || session == m_sessions.end()) continue;
            // A window that left detaches: the session keeps running.
            if (!it->second->handle(revents, **session)) m_attachments.erase(it);
        }
        pumpAttachments();
        for (std::size_t i = firstConnection; i < firstAttachment; ++i) {
            auto& connection = *m_connections[i - firstConnection];
            if (fds[i].revents & POLLOUT) connection.flush();
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) || connection.pending()) read(connection);
        }
        std::erase_if(m_connections, [](const auto& c) { return !c->open(); });
        // Whether the poll saw it or not: a client that connected since the
        // poll waits in a backlog, and an idle daemon would close its
        // connection with the listener.
        accept();
        if (idle() && (m_hadSession || std::chrono::steady_clock::now() >= startDeadline)) return {0, 0};
    }
}

void Daemon::accept()
{
    for (const auto& listener : m_listeners) {
        for (int fd = listener->accept(); fd >= 0; fd = listener->accept()) {
            m_connections.push_back(std::make_unique<IpcServerConnection>(fd));
        }
    }
}

void Daemon::read(IpcServerConnection& connection)
{
    ucred peer{};
    socklen_t size = sizeof peer;
    const pid_t caller = ::getsockopt(connection.fd(), SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 ? peer.pid : 0;
    IpcRequest request;
    for (int lines = 0; lines < MaxLinesPerRead; ++lines) {
        const IpcServerConnection::Read next = connection.nextRequest(request);
        if (next == IpcServerConnection::Read::None) return;
        if (next == IpcServerConnection::Read::Skipped) continue;
        std::string reply;
        try {
            reply = answer(request, caller);
        } catch (const std::exception& e) {
            // One call that fails (out of memory, no thread for a PTY) must
            // not end the sessions of all the others.
            reply = ipcError(request.id, IpcErrorCode::CallFailed, std::string("the call failed: ") + e.what());
        }
        connection.send(std::move(reply));
    }
}

std::string Daemon::answer(const IpcRequest& request, pid_t caller)
{
    // Before the type: a client of a session pane hears why, whatever it
    // sends (`afterglow ctl` subscribes to events, for one).
    if (request.target == ControlTarget || request.target == AfterglowProto::Run::Target) {
        return ipcError(request.id, IpcErrorCode::CallFailed, "a session pane takes no control calls until a window shows the session");
    }
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
    if (request.fn == Fn::ListSessions) return listSessions(request, caller);
    if (request.fn == Fn::KillSession) return killSession(request);
    if (request.fn == Fn::Attach) return attach(request, caller);
    if (request.fn == Fn::Detach) return detach(request);
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
    if (find(name) != m_sessions.end()) return ipcError(request.id, Error::SessionExists, "a session named '" + name + "' exists");
    auto session = std::make_unique<Session>(name, std::move(*options), m_fileLimit);
    const int wake = m_wakeFd;
    const auto exited = [wake] {
        const std::uint64_t one = 1;
        (void)!::write(wake, &one, sizeof one);
    };
    // The spawn waits for the exec of the program, up to the PTY's exec wait
    // (2 s), and the other connections wait with it. Only a program or a
    // directory on a mount that does not answer takes that long; past the
    // wait, the PTY reports a late failure as the exit of the pane.
    if (!session->addPane(m_nextPane++, paneSocket(), exited, &error)) {
        return ipcError(request.id, Error::SpawnFailed, "cannot start the program of session '" + name + "': " + error);
    }
    m_sessions.push_back(std::move(session));
    m_hadSession = true;
    return ipcReply(request.id.value_or(0), Json::Object{{"name", name}});
}

std::string Daemon::listSessions(const IpcRequest& request, pid_t caller)
{
    Json::Array sessions;
    for (const auto& session : m_sessions) {
        SessionInfo info = session->info(caller);
        info.attached = m_attachments.count(session->name()) != 0;
        sessions.push_back(toJson(info));
    }
    return ipcReply(request.id.value_or(0), std::move(sessions));
}

std::string Daemon::killSession(const IpcRequest& request)
{
    if (request.args.size() != 1 || !request.args[0].isString()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg, "killSession takes (name)");
    }
    const std::string& name = request.args[0].toString();
    const auto it = find(name);
    if (it == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    if (const auto attached = m_attachments.find(name); attached != m_attachments.end()) {
        attached->second->finish(Attach::EventType::SessionEnded, nullptr);
        m_attachments.erase(attached);
    }
    (*it)->kill();
    m_sessions.erase(it);
    return ipcReply(request.id.value_or(0), nullptr);
}

std::string Daemon::attach(const IpcRequest& request, pid_t caller)
{
    if (request.args.size() != 3 || !request.args[1].isString() || !request.args[2].isObject()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg, "attach takes (protocol, name, options)");
    }
    const auto protocol = request.args[0].toInteger();
    if (!protocol || *protocol != ProtocolVersion) {
        return ipcError(request.id, Error::VersionMismatch,
                        "the client speaks mux protocol " + (protocol ? std::to_string(*protocol) : std::string("?"))
                            + ", and the daemon speaks " + std::to_string(ProtocolVersion));
    }
    const std::string& name = request.args[1].toString();
    const auto session = find(name);
    if (session == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    // Only the process that calls may connect to the socket: a caller that
    // the kernel did not name could be anyone of the user.
    if (caller <= 0) return ipcError(request.id, IpcErrorCode::CallFailed, "the daemon cannot tell which process called");
    const Json::Value* takeover = request.args[2].find("takeover");
    if (takeover && !takeover->isBool()) return ipcError(request.id, IpcErrorCode::InvalidArg, "'takeover' is not a boolean");
    const auto attached = m_attachments.find(name);
    if (attached != m_attachments.end()) {
        if (!takeover || !takeover->toBool()) {
            return ipcError(request.id, Error::Attached, "another window shows session '" + name + "'");
        }
        attached->second->finish(Attach::EventType::TakenOver, session->get());
        m_attachments.erase(attached);
    }
    // Next to the daemon's socket, in the directory that only the user opens.
    const std::string& socket = paneSocket();
    std::string error;
    auto attachment = Attachment::listen(socket.substr(0, socket.rfind('/')), caller, &error);
    if (!attachment) return ipcError(request.id, IpcErrorCode::CallFailed, error);
    const std::string path = attachment->path();
    m_attachments[name] = std::move(attachment);
    return ipcReply(request.id.value_or(0), Json::Object{{"name", name}, {"socket", path}});
}

std::string Daemon::detach(const IpcRequest& request)
{
    if (request.args.size() != 1 || !request.args[0].isString()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg, "detach takes (name)");
    }
    const std::string& name = request.args[0].toString();
    const auto session = find(name);
    if (session == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    const auto attached = m_attachments.find(name);
    if (attached == m_attachments.end()) return ipcError(request.id, Error::NotAttached, "no window shows session '" + name + "'");
    attached->second->finish(Attach::EventType::Detached, session->get());
    m_attachments.erase(attached);
    return ipcReply(request.id.value_or(0), nullptr);
}

void Daemon::pumpAttachments()
{
    for (auto& [name, attachment] : m_attachments) {
        if (const auto session = find(name); session != m_sessions.end()) attachment->pump(**session);
    }
}

const std::string& Daemon::paneSocket() const
{
    // The first that a path still names: after logind removed the runtime
    // directory, the state directory's socket, which clients still reach.
    for (const auto& listener : m_listeners) {
        if (listener->stillOurs()) return listener->path();
    }
    return m_listeners.front()->path();
}

std::vector<std::unique_ptr<Session>>::iterator Daemon::find(const std::string& name)
{
    return std::find_if(m_sessions.begin(), m_sessions.end(), [&](const auto& s) { return s->name() == name; });
}

std::string Daemon::freeName()
{
    // The numbers only go up: a session that `afterglow mux kill` listed and
    // that ended before the kill never has a new session in its name.
    while (true) {
        std::string name = std::to_string(m_nextName++);
        if (find(name) == m_sessions.end()) return name;
    }
}

void Daemon::reap()
{
    std::erase_if(m_sessions, [this](const auto& session) {
        std::vector<std::pair<std::uint32_t, int>> closed;
        const bool ended = session->reap(&closed);
        const auto attached = m_attachments.find(session->name());
        if (attached == m_attachments.end()) return ended;
        for (const auto& [pane, status] : closed) attached->second->paneClosed(pane, status);
        if (ended) {
            attached->second->finish(Attach::EventType::SessionEnded, nullptr);
            m_attachments.erase(attached);
        }
        return ended;
    });
}

bool Daemon::idle() const
{
    return m_sessions.empty() && m_connections.empty();
}

} // namespace AfterglowMux
