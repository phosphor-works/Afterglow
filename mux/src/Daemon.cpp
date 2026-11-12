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
#include <string_view>
#include <exception>
#include <functional>

#include <dirent.h>
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

/// Removes the files named as Attachment::listen() names its sockets in
/// @p directory.
void removeAttachSockets(const std::string& directory)
{
    DIR* dir = ::opendir(directory.c_str());
    if (!dir) return;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string_view name = entry->d_name;
        if (name.size() == 28 && name.starts_with("attach-") && name.ends_with(".sock")) {
            ::unlinkat(::dirfd(dir), entry->d_name, 0);
        }
    }
    ::closedir(dir);
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
    // Their closed panes too, whose reader threads may still wake the daemon:
    // before the wake descriptor closes.
    m_attachments.clear();
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
    // The attach sockets that a daemon before this one left when it was
    // killed, in the runtime directory (its socket is "socket"): that
    // directory is of this computer, and this daemon holds its lock. The
    // state directory can be shared with other computers (NFS), whose
    // daemons wait for windows there.
    for (const auto& listener : m_listeners) {
        const std::string& path = listener->path();
        if (path.ends_with("/socket")) removeAttachSockets(path.substr(0, path.rfind('/')));
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
        for (const auto& attachment : m_attachments) {
            fds.push_back(attachment->pollFd());
            pending = pending || attachment->busy();
            if (const int left = attachment->timeoutMs(); left >= 0 && (connectWait < 0 || left < connectWait)) {
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
        // A window that does not connect to its attach socket in time loses
        // it, and a finished one that cannot send what waits is let go.
        if (connectWait >= 0 && (timeout < 0 || connectWait + 1 < timeout)) timeout = connectWait + 1;
        const int ready = ::poll(fds.data(), fds.size(), timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "afterglow-mux: poll failed: %s\n", std::strerror(errno));
            return {1, 0};
        }
        // Paired with what the poll saw before anything below adds, finishes
        // or removes one: reap() only finishes them, and the objects stay.
        std::vector<std::pair<Attachment*, short>> attached;
        for (std::size_t i = 0; i < m_attachments.size(); ++i) {
            attached.emplace_back(m_attachments[i].get(), fds[firstAttachment + i].revents);
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
        std::vector<Attachment*> over;
        for (const auto& [attachment, revents] : attached) {
            const auto session = attachment->finishing() ? m_sessions.end() : find(attachment->session());
            Session* const shown = session == m_sessions.end() ? nullptr : session->get();
            // A window that left detaches: the session keeps running.
            const bool keep = guarded(*attachment, [&] { return attachment->handle(revents, shown); });
            if (!keep) over.push_back(attachment);
            // The window closed and Mux.onClose is kill. Its own window
            // sends it, so a window that another took the session from
            // cannot end it. A window that closed the last pane of the
            // session ends it too: as when its last program exits.
            if ((attachment->takeKill() || (shown && shown->empty())) && shown) endSession(session);
        }
        for (Attachment* attachment : over) {
            // The calls that the window did not get get the answers of a
            // session that no window shows.
            const auto session = find(attachment->session());
            guarded(*attachment, [&] {
                attachment->returnHostCalls(session == m_sessions.end() ? nullptr : session->get(), true);
                return true;
            });
        }
        std::erase_if(m_attachments, [&over](const auto& a) { return std::find(over.begin(), over.end(), a.get()) != over.end(); });
        pumpAttachments();
        syncAttached();
        for (std::size_t i = firstConnection; i < firstAttachment; ++i) {
            auto& connection = *m_connections[i - firstConnection];
            if (fds[i].revents & POLLOUT) connection.flush();
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) || connection.pending()) read(connection);
        }
        std::erase_if(m_connections, [](const auto& c) { return !c->open(); });
        // A call can start a pane, end one or move panes: the window that
        // shows the session hears of it now, not at the next wake, which a
        // program that prints nothing never gives.
        pumpAttachments();
        syncAttached();
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
        return ipcError(request.id, IpcErrorCode::CallFailed, "a session pane takes no control calls: the mux daemon answers only mux calls");
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
    if (request.fn == Fn::PaneDirectory) return paneDirectory(request);
    if (request.fn == Fn::NewPane) return newPane(request);
    if (request.fn == Fn::PaneJob) return paneJob(request, caller);
    if (request.fn == Fn::BreakOut) return breakOut(request);
    if (request.fn == Fn::PaneSession) return paneSession(request);
    return ipcError(request.id, IpcErrorCode::NoSuchFn, "no function '" + request.fn + "' on target '" + request.target + "'");
}

std::optional<std::string> Daemon::versionError(const IpcRequest& request)
{
    const auto protocol = request.args.empty() ? std::nullopt : request.args[0].toInteger();
    if (protocol && *protocol == ProtocolVersion) return std::nullopt;
    return ipcError(request.id, Error::VersionMismatch,
                    "the client speaks mux protocol " + (protocol ? std::to_string(*protocol) : std::string("?"))
                        + ", and the daemon speaks " + std::to_string(ProtocolVersion));
}

std::string Daemon::newSession(const IpcRequest& request)
{
    if (request.args.size() != 2) return ipcError(request.id, IpcErrorCode::InvalidArg, "newSession takes (protocol, options)");
    if (auto error = versionError(request)) return *error;
    std::string error;
    auto options = sessionOptions(request.args[1], &error);
    if (!options) return ipcError(request.id, IpcErrorCode::InvalidArg, error);
    std::string name = options->name.empty() ? freeName() : options->name;
    if (find(name) != m_sessions.end()) return ipcError(request.id, Error::SessionExists, "a session named '" + name + "' exists");
    auto session = std::make_unique<Session>(name, std::move(*options), m_fileLimit, m_images);
    // The spawn waits for the exec of the program, up to the PTY's exec wait
    // (2 s), and the other connections wait with it. Only a program or a
    // directory on a mount that does not answer takes that long; past the
    // wait, the PTY reports a late failure as the exit of the pane.
    if (!session->addPane(m_nextPane++, session->firstLaunch(), session->firstSize(), paneSocket(), waker(), &error)) {
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
        // A window that the daemon still waits for may never come: until it
        // connects, `attach --no-takeover` may take the session.
        const Attachment* attachment = shown(session->name());
        info.attached = attachment && attachment->connected();
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
    endSession(it);
    return ipcReply(request.id.value_or(0), nullptr);
}

void Daemon::endSession(std::vector<std::unique_ptr<Session>>::iterator session)
{
    if (Attachment* attachment = shown((*session)->name())) {
        guarded(*attachment, [&] {
            attachment->finish(Attach::EventType::SessionEnded);
            return true;
        });
    }
    (*session)->kill();
    m_sessions.erase(session);
}

std::string Daemon::attach(const IpcRequest& request, pid_t caller)
{
    if (request.args.size() != 3 || !request.args[1].isString() || !request.args[2].isObject()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg, "attach takes (protocol, name, options)");
    }
    if (auto error = versionError(request)) return *error;
    const std::string& name = request.args[1].toString();
    const auto session = find(name);
    if (session == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    // Only the process that calls may connect to the socket: a caller that
    // the kernel did not name could be anyone of the user.
    if (caller <= 0) return ipcError(request.id, IpcErrorCode::CallFailed, "the daemon cannot tell which process called");
    // A key that this daemon does not know is one that the window counts on.
    for (const auto& [key, value] : request.args[2].toObject()) {
        if (key != AttachOption::Takeover && key != AttachOption::EventQueue && key != AttachOption::History) {
            return ipcError(request.id, IpcErrorCode::InvalidArg, "the attach option '" + key + "' is unknown");
        }
    }
    const Json::Value* takeover = request.args[2].find(AttachOption::Takeover);
    if (takeover && !takeover->isBool()) return ipcError(request.id, IpcErrorCode::InvalidArg, "'takeover' is not a boolean");
    // Mux.eventQueue of the window, in bytes.
    std::size_t eventQueue = std::size_t(DefaultEventQueue);
    if (const Json::Value* queue = request.args[2].find(AttachOption::EventQueue)) {
        const auto bytes = queue->toInteger();
        if (!bytes || *bytes < MinEventQueue || *bytes > MaxEventQueue) {
            return ipcError(request.id, IpcErrorCode::InvalidArg,
                            "'eventQueue' is not " + std::to_string(MinEventQueue >> 20) + " to " + std::to_string(MaxEventQueue >> 20)
                                + " MiB in bytes");
        }
        eventQueue = std::size_t(*bytes);
    }
    // Terminal.scrollback of the window, in rows.
    std::size_t history = ~std::size_t(0);
    if (const Json::Value* rows = request.args[2].find(AttachOption::History)) {
        const auto count = rows->toInteger();
        if (!count || *count < 0 || *count > MaxHistory) {
            return ipcError(request.id, IpcErrorCode::InvalidArg, "'history' is not 0 to " + std::to_string(MaxHistory) + " rows");
        }
        history = std::size_t(*count);
    }
    Attachment* const previous = shown(name);
    if (previous && (!takeover || !takeover->toBool())) {
        return ipcError(request.id, Error::Attached, "another window shows session '" + name + "'");
    }
    // Next to the daemon's socket, in the directory that only the user
    // opens. Made before the takeover: a socket that cannot be made leaves
    // the session in the window that shows it.
    const std::string& socket = paneSocket();
    std::string error;
    auto attachment = Attachment::listen(socket.substr(0, socket.rfind('/')), name, caller, &error);
    if (!attachment) return ipcError(request.id, IpcErrorCode::CallFailed, error);
    if (previous) {
        guarded(*previous, [&] {
            previous->pump(session->get());
            previous->finish(Attach::EventType::TakenOver, session->get());
            return true;
        });
    }
    attachment->setEventQueue(eventQueue);
    attachment->setReplicaHistory(history);
    const std::string path = attachment->path();
    m_attachments.push_back(std::move(attachment));
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
    Attachment* const attachment = shown(name);
    if (!attachment) return ipcError(request.id, Error::NotAttached, "no window shows session '" + name + "'");
    // A window that did not connect yet hears it when it does.
    guarded(*attachment, [&] {
        attachment->pump(session->get());
        attachment->finish(Attach::EventType::Detached, session->get());
        return true;
    });
    return ipcReply(request.id.value_or(0), nullptr);
}

std::string Daemon::paneDirectory(const IpcRequest& request)
{
    const auto id = request.args.size() == 2 && request.args[0].isString() ? request.args[1].toInteger() : std::nullopt;
    if (!id || *id < 0 || *id > 0xffffffffLL) return ipcError(request.id, IpcErrorCode::InvalidArg, "paneDirectory takes (name, pane)");
    const std::string& name = request.args[0].toString();
    const auto session = find(name);
    if (session == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    const MuxPane* pane = (*session)->pane(std::uint32_t(*id));
    if (!pane) return ipcError(request.id, Error::NoSuchPane, "session '" + name + "' has no pane " + std::to_string(*id));
    return ipcReply(request.id.value_or(0), pane->directory());
}

std::string Daemon::newPane(const IpcRequest& request)
{
    if (request.args.size() != 4 || !request.args[1].isString()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg, "newPane takes (protocol, name, launch, size)");
    }
    if (auto error = versionError(request)) return *error;
    const std::string& name = request.args[1].toString();
    const auto session = find(name);
    if (session == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    std::string error;
    const auto launch = launchFrom(request.args[2], &error);
    if (!launch) return ipcError(request.id, IpcErrorCode::InvalidArg, error);
    const auto size = paneSizeFrom(request.args[3], &error);
    if (!size) return ipcError(request.id, IpcErrorCode::InvalidArg, error);
    // The window that asked binds the pane before its first frame, which
    // the next pass sends: it knows the ID from this answer.
    const std::uint32_t id = m_nextPane++;
    if (!(*session)->addPane(id, *launch, *size, paneSocket(), waker(), &error)) {
        return ipcError(request.id, Error::SpawnFailed, "cannot start the program of a pane of session '" + name + "': " + error);
    }
    return ipcReply(request.id.value_or(0), Json::Object{{"pane", std::int64_t(id)}});
}

std::string Daemon::paneJob(const IpcRequest& request, pid_t caller)
{
    const auto id = request.args.size() == 2 && request.args[0].isString() ? request.args[1].toInteger() : std::nullopt;
    if (!id || *id < 0 || *id > 0xffffffffLL) return ipcError(request.id, IpcErrorCode::InvalidArg, "paneJob takes (name, pane)");
    const std::string& name = request.args[0].toString();
    const auto session = find(name);
    if (session == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    const MuxPane* pane = (*session)->pane(std::uint32_t(*id));
    if (!pane) return ipcError(request.id, Error::NoSuchPane, "session '" + name + "' has no pane " + std::to_string(*id));
    return ipcReply(request.id.value_or(0), pane->foregroundJob(caller));
}

std::string Daemon::breakOut(const IpcRequest& request)
{
    if (request.args.size() != 3 || !request.args[1].isString()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg, "breakOut takes (protocol, name, layout)");
    }
    if (auto error = versionError(request)) return *error;
    const std::string& name = request.args[1].toString();
    const auto from = find(name);
    if (from == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    std::string error;
    auto layout = layoutFrom(request.args[2], &error);
    if (!layout) return ipcError(request.id, IpcErrorCode::InvalidArg, error);
    const std::vector<std::uint32_t> ids = layout->panes();
    for (const std::uint32_t id : ids) {
        if (!(*from)->pane(id)) return ipcError(request.id, Error::NoSuchPane, "session '" + name + "' has no pane " + std::to_string(id));
    }
    if (ids.size() >= (*from)->panes().size()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg,
                        "these are all the panes of session '" + name + "': a new session would only give them another name");
    }
    const std::string fresh = freeName();
    auto session = std::make_unique<Session>(fresh, (*from)->options(), m_fileLimit, m_images);
    Attachment* const attachment = shown(name);
    for (const std::uint32_t id : ids) {
        std::unique_ptr<MuxPane> pane = (*from)->take(id);
        if (attachment) {
            guarded(*attachment, [&] {
                attachment->paneRemoved(id, pane.get());
                return true;
            });
        }
        session->adopt(std::move(pane));
    }
    session->setLayout(std::move(*layout));
    m_sessions.push_back(std::move(session));
    return ipcReply(request.id.value_or(0), Json::Object{{"name", fresh}});
}

std::string Daemon::paneSession(const IpcRequest& request)
{
    const auto id = request.args.size() == 1 ? request.args[0].toInteger() : std::nullopt;
    if (!id || *id < 0 || *id > 0xffffffffLL) return ipcError(request.id, IpcErrorCode::InvalidArg, "paneSession takes (pane)");
    for (const auto& session : m_sessions) {
        if (session->pane(std::uint32_t(*id))) return ipcReply(request.id.value_or(0), session->name());
    }
    return ipcError(request.id, Error::NoSuchPane, "no session has pane " + std::to_string(*id));
}

std::function<void()> Daemon::waker() const
{
    return [wake = m_wakeFd] {
        const std::uint64_t one = 1;
        (void)!::write(wake, &one, sizeof one);
    };
}

void Daemon::syncAttached()
{
    for (const auto& session : m_sessions) {
        const Attachment* attachment = shown(session->name());
        const bool showing = attachment && attachment->showing();
        for (const auto& pane : session->panes()) {
            try {
                pane->setWindow(showing ? attachment->serial() : 0, showing ? attachment->eventQueue() : 0);
            } catch (const std::exception& e) {
                // As for a window (guarded): the answers to the calls of a
                // window that went, out of memory, must not end the sessions
                // of all the others. The next pass tries again.
                std::fprintf(stderr, "afterglow-mux: a pane of session %s failed: %s\n", session->name().c_str(), e.what());
            }
        }
    }
}

void Daemon::pumpAttachments()
{
    for (const auto& attachment : m_attachments) {
        const auto session = attachment->finishing() ? m_sessions.end() : find(attachment->session());
        guarded(*attachment, [&] {
            attachment->pump(session == m_sessions.end() ? nullptr : session->get());
            // A window that failed here: its calls get their answers before
            // syncAttached() answers the newer ones of its panes.
            if (attachment->failed()) attachment->returnHostCalls(session == m_sessions.end() ? nullptr : session->get(), true);
            return true;
        });
    }
}

Attachment* Daemon::shown(const std::string& name) const
{
    for (const auto& attachment : m_attachments) {
        if (attachment->session() == name && !attachment->finishing()) return attachment.get();
    }
    return nullptr;
}

bool Daemon::guarded(Attachment& attachment, const std::function<bool()>& work)
{
    try {
        return work();
    } catch (const std::exception& e) {
        // As for a call: one window that fails (out of memory for a frame)
        // must not end the sessions of all the others. That window loses
        // its session, which keeps running.
        std::fprintf(stderr, "afterglow-mux: the window of session %s failed: %s\n", attachment.session().c_str(), e.what());
        // The calls that waited for it get the answers of a session that no
        // window shows. Out of memory again, they get none.
        const auto session = find(attachment.session());
        try {
            attachment.finish(Attach::EventType::Detached, session == m_sessions.end() ? nullptr : session->get());
        } catch (const std::exception&) {
        }
        return false;
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
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        Session& session = **it;
        Attachment* const attachment = shown(session.name());
        bool exited = false;
        for (const auto& pane : session.panes()) exited = exited || pane->exited();
        if (!exited) {
            ++it;
            continue;
        }
        if (attachment) {
            // The output that came before an exit goes in a frame while the
            // pane is still in the session, then the pane's last frames.
            guarded(*attachment, [&] {
                attachment->pump(&session);
                return true;
            });
        }
        for (auto& pane : session.reap()) {
            if (!attachment) continue;
            guarded(*attachment, [&] {
                attachment->paneClosed(std::move(pane));
                return true;
            });
        }
        if (!session.empty()) {
            ++it;
            continue;
        }
        if (attachment) {
            guarded(*attachment, [&] {
                attachment->finish(Attach::EventType::SessionEnded);
                return true;
            });
        }
        it = m_sessions.erase(it);
    }
}

bool Daemon::idle() const
{
    // A finished window still gets its last frames and event.
    return m_sessions.empty() && m_connections.empty() && m_attachments.empty();
}

} // namespace AfterglowMux
