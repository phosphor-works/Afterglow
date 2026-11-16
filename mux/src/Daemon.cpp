// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Daemon.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Run.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <string_view>

#include <fcntl.h>
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

/// The bytes of @p path, empty when it cannot be read.
std::string readAll(const std::string& path)
{
    std::string bytes;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return bytes;
    char buffer[65536];
    for (ssize_t n; (n = ::read(fd, buffer, sizeof buffer)) != 0;) {
        if (n > 0) bytes.append(buffer, std::size_t(n));
        else if (errno != EINTR) break;
    }
    ::close(fd);
    return bytes;
}

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
    // Their forwards are cancelled, and their clients hear why. Nothing may
    // leave a destructor, which may run as an exception unwinds.
    try {
        endAllSsh();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "aglow-mux: cannot end the ssh sessions: %s\n", e.what());
    }
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
        std::fprintf(stderr, "aglow-mux: %s\n", reason.c_str());
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
            if (error) *error = "another aglow-mux listens on " + socket.path;
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
    // This daemon owns the directories of its sockets now: the sockets of ssh
    // sessions that a daemon left when it was killed go. Not in a directory
    // that it left out, which may not be the user's.
    for (const auto& listener : m_listeners) {
        const std::string& path = listener->path();
        removeStaleSshSockets(path.substr(0, path.rfind('/')));
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
    std::vector<SshWatch> sshWatches;
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
            const bool reading = reads(*connection);
            const short events = short((reading && connection->wantsRead() ? POLLIN : 0) | (connection->wantsWrite() ? POLLOUT : 0));
            fds.push_back({connection->fd(), events, 0});
            pending = pending || (reading && connection->pending());
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
        const std::size_t firstSsh = fds.size();
        addSshFds(fds, sshWatches);
        // Lines that wait in a buffer get their turn at once. Until the
        // first session, the start wait bounds the poll: then an idle daemon
        // ends even when nothing wakes it. After the wait, only a connection
        // keeps it, and its close wakes the poll.
        int timeout = -1;
        if (std::exchange(m_again, false) || pending) {
            timeout = 0;
        } else if (!m_hadSession) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(startDeadline - std::chrono::steady_clock::now());
            if (left.count() >= 0) timeout = int(left.count()) + 1;
        }
        // A window that does not start its stream on the connection of its
        // attach call in time loses the session, and a finished one that
        // cannot send what waits is let go.
        if (connectWait >= 0 && (timeout < 0 || connectWait + 1 < timeout)) timeout = connectWait + 1;
        // An event of `afterglow ctl watch` that waits for its interval.
        if (const int due = watchDueMs(); due >= 0 && (timeout < 0 || due < timeout)) timeout = due;
        // An `ssh -O cancel` that no pidfd shows the end of, or that runs too
        // long.
        if (const int due = sshDueMs(); due >= 0 && (timeout < 0 || due < timeout)) timeout = due;
        const int ready = ::poll(fds.data(), fds.size(), timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "aglow-mux: poll failed: %s\n", std::strerror(errno));
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
            const auto session = find(attachment->session());
            Session* const own = session == m_sessions.end() ? nullptr : session->get();
            const bool finishing = attachment->finishing();
            // A window that left detaches: the session keeps running.
            const bool keep = guarded(*attachment, [&] { return attachment->handle(revents, own); });
            if (!keep) over.push_back(attachment);
            if (!own) continue;
            // What a window that no longer shows the session sent before it
            // heard.
            if (finishing) {
                // As the window's own input: a failure (out of memory) costs
                // that window's late input, not the daemon.
                guarded(*attachment, [&] {
                    applyLate(*attachment, *own);
                    return true;
                });
                // As when its last program exits. Outside the guard: late
                // input that failed after it closed the last pane leaves no
                // empty session behind.
                if (own->empty()) endSession(session);
                continue;
            }
            // The window closed and Mux.onClose is kill. Its own window
            // sends it, so a window that another took the session from
            // cannot end it. A window that closed the last pane of the
            // session ends it too: as when its last program exits.
            if (attachment->takeKill() || own->empty()) endSession(session);
        }
        for (Attachment* attachment : over) {
            // What the window sent its command clients before it went, the
            // end of a run, still reaches them. So do its answers to the
            // calls that it got: a runAction that closes the window, for one.
            deliverRelayFrames(*attachment);
            passReplies(*attachment);
            // The calls that the window did not get get the answers of a
            // session that no window shows.
            const auto session = find(attachment->session());
            guarded(*attachment, [&] {
                attachment->returnHostCalls(session == m_sessions.end() ? nullptr : session->get(), true);
                return true;
            });
        }
        std::erase_if(m_attachments, [&over](const auto& a) { return std::find(over.begin(), over.end(), a.get()) != over.end(); });
        // The ssh sessions before the windows' answers: a client that came
        // goes to its window with them. Each step below fails item by item
        // (contained()).
        handleSshFds(fds.data() + firstSsh, sshWatches);
        // Before the reads below, whose calls may wait for these windows.
        answerForwarded();
        answerRelays();
        pumpAttachments();
        syncAttached();
        bool answered = false;
        for (std::size_t i = firstConnection; i < firstAttachment; ++i) {
            auto& connection = *m_connections[i - firstConnection];
            if (fds[i].revents & POLLOUT) connection.flush();
            if (!reads(connection)) {
                // A client that hung up while its window had no room, or
                // while its connection closes, has nothing more to say:
                // its next poll would not wait.
                if (fds[i].revents & (POLLHUP | POLLERR)) connection.close();
                else if (m_closing.contains(&connection) && !connection.wantsWrite()) connection.close();
                continue;
            }
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) || connection.pending()) {
                // A call that fails gets its error (read()). What fails
                // outside a call (the lines of a command client, the send of
                // an answer, an attach) costs the client its connection, and
                // an attach that did not get its answer out its window.
                bool called = false;
                if (!contained("a client", [&] { called = read(connection); })) {
                    m_attaching.reset();
                    connection.close();
                    called = true;
                }
                answered = called || answered;
            }
            // A client that closed its connection whole hears nothing more:
            // one that keepAtEnd() holds for an answer would wake each poll.
            if ((fds[i].revents & (POLLHUP | POLLERR)) && !connection.wantsRead() && !connection.pending()) connection.close();
        }
        // A call can start a pane, end one, move panes or attach a window:
        // the window that shows the session hears of it now, not at the next
        // wake, which a program that prints nothing never gives. Without a
        // call, nothing changed since the pass above.
        if (answered) pumpAttachments();
        // A pane that closed, or a window that turned remote commands off,
        // ends ssh sessions.
        pruneSsh();
        // The events of `afterglow ctl watch` of what changed.
        pumpWatch();
        if (answered) syncAttached();
        // After each step that sends: a connection that a send closed, or that
        // closes once its output went and has none left, goes now. No poll
        // would wake for it.
        for (const auto& connection : m_connections) {
            if (m_closing.contains(connection.get()) && !connection->wantsWrite()) connection->close();
            if (!connection->open()) contained("the end of a connection", [&] { forget(connection.get()); });
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
            // A client that fails to come in (out of memory) is dropped, and
            // the others still come.
            std::unique_ptr<IpcServerConnection> owned;
            const bool added = contained("a new client", [&] {
                owned = std::make_unique<IpcServerConnection>(fd);
                m_connections.push_back(std::move(owned));
            });
            // Unless a connection holds it.
            if (!added && !owned) ::close(fd);
        }
    }
}

bool Daemon::read(IpcServerConnection& connection)
{
    // The bytes of a command client go to its window: as a call, they
    // want the windows pumped.
    if (m_relayOf.contains(&connection)) {
        readRelay(connection);
        return true;
    }
    ucred peer{};
    socklen_t size = sizeof peer;
    const pid_t caller = ::getsockopt(connection.fd(), SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 ? peer.pid : 0;
    IpcRequest request;
    std::string line;
    bool answered = false;
    for (int lines = 0; lines < MaxLinesPerRead; ++lines) {
        const IpcServerConnection::Read next = connection.nextRequest(request, &line);
        if (next == IpcServerConnection::Read::None) return answered;
        if (next == IpcServerConnection::Read::Skipped) continue;
        answered = true;
        std::string reply;
        try {
            // Before the type: a client of the commands target hears from
            // the window whatever it sends (`aglow run` subscribes to its
            // output, for one).
            if (request.target == AfterglowProto::Run::Target) reply = startRelay(request, std::move(line), caller, connection);
            else reply = answer(request, caller, connection);
        } catch (const std::exception& e) {
            // One call that fails (out of memory, no thread for a PTY) must
            // not end the sessions of all the others.
            reply = ipcError(request.id, IpcErrorCode::CallFailed, std::string("the call failed: ") + e.what());
        }
        // A window answers it later, or the call sent its answer itself.
        if (!reply.empty()) connection.send(std::move(reply));
        // The connection is the window's now: its other lines go there as
        // they are.
        if (m_relayOf.contains(&connection)) {
            readRelay(connection);
            return true;
        }
        if (m_attaching) {
            // The attach call made this connection the window's stream: it
            // is the attachment's now, and reads no more lines here.
            std::unique_ptr<Attachment> attachment = std::move(m_attaching);
            std::string unsent;
            const bool open = connection.open();
            const int fd = connection.release(&unsent);
            if (fd < 0) {
                // It sent more after its call, which the stream would lose,
                // or it went before the answer did: the window that shows
                // the session keeps it.
                if (open) {
                    std::fprintf(stderr, "aglow-mux: the window of session %s sent its stream before the answer to its attach\n",
                                 attachment->session().c_str());
                } else {
                    std::fprintf(stderr, "aglow-mux: the window of session %s went before the answer to its attach\n",
                                 attachment->session().c_str());
                }
                connection.close();
                return true;
            }
            // Only a window whose connection becomes its stream takes the
            // session from the window that shows it: one whose connection
            // cannot leaves the session where it is, and its connection
            // closes with it.
            attachment->adopt(fd, std::move(unsent));
            if (attachment->failed()) {
                std::fprintf(stderr, "aglow-mux: the stream of a window of session %s cannot be made non-blocking: %s\n",
                             attachment->session().c_str(), std::strerror(errno));
                return true;
            }
            if (Attachment* previous = shown(attachment->session())) takeOver(*previous);
            m_attachments.push_back(std::move(attachment));
            return true;
        }
    }
    return answered;
}

std::string Daemon::answer(const IpcRequest& request, pid_t caller, IpcServerConnection& connection)
{
    if (request.type == "unsubscribe") return unsubscribe(request, connection);
    if (request.target == ControlTarget) return control(request, connection, caller);
    if (request.type != "call") {
        return ipcError(request.id, IpcErrorCode::MalformedRequest, "the mux daemon answers only calls, not '" + request.type + "'");
    }
    if (request.target != Target) {
        return ipcError(request.id, IpcErrorCode::NoSuchTarget, "no target '" + request.target + "'");
    }
    if (request.fn == Fn::Version) {
        return ipcReply(request.id.value_or(0), Json::Object{{"version", AFTERGLOW_VERSION}, {"protocol", ProtocolVersion}});
    }
    if (request.fn == Fn::NewSession) return newSession(request, caller);
    if (request.fn == Fn::ListSessions) return listSessions(request, caller);
    if (request.fn == Fn::KillSession) return killSession(request);
    if (request.fn == Fn::Attach) return attach(request);
    if (request.fn == Fn::Detach) return detach(request);
    if (request.fn == Fn::PaneDirectory) return paneDirectory(request);
    if (request.fn == Fn::NewPane) return newPane(request);
    if (request.fn == Fn::PaneJob) return paneJob(request, caller);
    if (request.fn == Fn::BreakOut) return breakOut(request);
    if (request.fn == Fn::JoinPane) return joinPane(request);
    if (request.fn == Fn::PaneSession) return paneSession(request);
    if (request.fn == Fn::SelectSession || request.fn == Fn::SelectPane) return select(request, connection);
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

std::string Daemon::newSession(const IpcRequest& request, pid_t caller)
{
    if (request.args.size() != 2) return ipcError(request.id, IpcErrorCode::InvalidArg, "newSession takes (protocol, options)");
    if (auto error = versionError(request)) return *error;
    std::string error;
    auto options = sessionOptions(request.args[1], &error);
    if (!options) return ipcError(request.id, IpcErrorCode::InvalidArg, error);
    if (options->callerEnvironment) {
        // The caller's own (the proxy in the user's ssh login), with the
        // client's variables over it.
        std::string callerEnviron = caller > 0 ? readAll("/proc/" + std::to_string(caller) + "/environ") : std::string();
        if (callerEnviron.empty()) return ipcError(request.id, IpcErrorCode::CallFailed, "the daemon cannot read the environment of the caller");
        std::vector<std::pair<std::string, std::string>> merged;
        for (std::size_t at = 0; at < callerEnviron.size();) {
            const std::size_t end = std::min(callerEnviron.find('\0', at), callerEnviron.size());
            const std::string_view entry(callerEnviron.data() + at, end - at);
            const std::size_t eq = entry.find('=');
            if (eq != std::string_view::npos && eq > 0) merged.emplace_back(std::string(entry.substr(0, eq)), std::string(entry.substr(eq + 1)));
            at = end + 1;
        }
        // The last value of each name of the client's, in one pass over
        // each list: a request line (1 MiB) can hold many variables.
        const auto& client = options->environment;
        std::map<std::string_view, std::size_t> last;
        for (std::size_t i = 0; i < client.size(); ++i) last[client[i].first] = i;
        std::erase_if(merged, [&last](const auto& variable) { return last.contains(variable.first); });
        for (std::size_t i = 0; i < client.size(); ++i) {
            if (last[client[i].first] == i) merged.push_back(client[i]);
        }
        options->environment = std::move(merged);
    }
    std::string name = options->name.empty() ? freeName() : options->name;
    if (find(name) != m_sessions.end()) return ipcError(request.id, Error::SessionExists, "a session named '" + name + "' exists");
    auto session = std::make_unique<Session>(name, std::move(*options), m_fileLimit, m_images);
    session->setSerial(++m_lastSessionSerial);
    // The spawn waits for the exec of the program, up to the PTY's exec wait
    // (2 s), and the other connections wait with it. Only a program or a
    // directory on a mount that does not answer takes that long; past the
    // wait, the PTY reports a late failure as the exit of the pane.
    if (!session->addPane(nextPaneId(), session->firstLaunch(), session->firstSize(), paneSocket(), 0, 0, waker(), &error)) {
        return ipcError(request.id, Error::SpawnFailed, "cannot start the program of session '" + name + "': " + error);
    }
    const std::uint32_t first = session->panes().front()->id();
    m_sessions.push_back(std::move(session));
    m_hadSession = true;
    return ipcReply(request.id.value_or(0), Json::Object{{"name", name}, {"pane", std::int64_t(first)}});
}

std::string Daemon::listSessions(const IpcRequest& request, pid_t caller)
{
    Json::Array sessions;
    for (const auto& session : m_sessions) {
        SessionInfo info = session->info(caller);
        // Also a window that the daemon still waits for, which may never
        // come: until its connect wait ends, an attach without a takeover is
        // refused (attach()).
        info.attached = shown(session->name()) != nullptr;
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
    flushWatch(**session);
    if (Attachment* attachment = shown((*session)->name())) {
        guarded(*attachment, [&] {
            attachment->finish(Attach::EventType::SessionEnded);
            return true;
        });
    }
    (*session)->kill();
    m_sessions.erase(session);
}

std::string Daemon::attach(const IpcRequest& request)
{
    if (request.args.size() != 3 || !request.args[1].isString() || !request.args[2].isObject()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg, "attach takes (protocol, name, options)");
    }
    if (auto error = versionError(request)) return *error;
    const std::string& name = request.args[1].toString();
    const auto session = find(name);
    if (session == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
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
    auto attachment = std::make_unique<Attachment>(name);
    // The window names itself by its serial in the calls that change the
    // session's panes (windowRefusal).
    std::string reply = ipcReply(request.id.value_or(0), Json::Object{{"name", name},
                                                                       {"window", std::int64_t(attachment->serial())},
                                                                       {"environment", environmentJson((*session)->options().environment)}});
    // read() moves it to the attachments once the answer went, and only then
    // takes the session from the window that shows it (takeOver()): a
    // connection that cannot be the stream leaves the session where it is.
    m_attachments.reserve(m_attachments.size() + 1);
    attachment->setEventQueue(eventQueue);
    attachment->setReplicaHistory(history);
    attachment->setPaneFinder([this](std::uint32_t id) { return findPane(id); });
    // read() gives it this connection once the answer went.
    m_attaching = std::move(attachment);
    return reply;
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
    // A window that did not connect yet hears it when it does. What the
    // window sent before the call counts, its last layout among it, for the
    // next window.
    guarded(*attachment, [&] {
        attachment->catchUp(**session);
        attachment->pump(session->get());
        attachment->finish(Attach::EventType::Detached, session->get());
        return true;
    });
    // The window closed the last pane before the call: the session ends, as
    // when its last program exits. Its late input (applyLate) would end it
    // too, but an attachment that failed meanwhile has none. So does a window
    // that closed with Mux.onClose kill before the call (catchUp() read its
    // Kill): a finished window's Kill is read no more.
    if (attachment->takeKill() || (*session)->empty()) endSession(session);
    return ipcReply(request.id.value_or(0), nullptr);
}

void Daemon::takeOver(Attachment& previous)
{
    const auto session = find(previous.session());
    Session* const own = session == m_sessions.end() ? nullptr : session->get();
    guarded(previous, [&] {
        // What the window sent before the call, its last layout among it,
        // counts: the new window starts from it. A Kill among it ends no
        // session that a new window takes: that window's user wants it.
        if (own) previous.catchUp(*own);
        previous.pump(own);
        previous.finish(Attach::EventType::TakenOver, own);
        return true;
    });
}

const MuxPane* Daemon::paneOfCall(const IpcRequest& request, const char* usage, std::string& refusal)
{
    const auto id = request.args.size() == 2 && request.args[0].isString() ? paneIdFrom(request.args[1]) : std::nullopt;
    if (!id) {
        refusal = ipcError(request.id, IpcErrorCode::InvalidArg, usage);
        return nullptr;
    }
    const std::string& name = request.args[0].toString();
    const auto session = find(name);
    if (session == m_sessions.end()) {
        refusal = ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
        return nullptr;
    }
    const MuxPane* pane = (*session)->pane(*id);
    if (!pane) refusal = ipcError(request.id, Error::NoSuchPane, "session '" + name + "' has no pane " + std::to_string(*id));
    return pane;
}

std::string Daemon::paneDirectory(const IpcRequest& request)
{
    std::string refusal;
    const MuxPane* pane = paneOfCall(request, "paneDirectory takes (name, pane)", refusal);
    return pane ? ipcReply(request.id.value_or(0), pane->directory()) : refusal;
}

std::string Daemon::windowRefusal(const IpcRequest& request, const std::string& name, std::size_t windowArg) const
{
    // A call that names a window is that window's only while it shows the
    // session. A window that another took the session from may call before
    // it hears of the takeover: its panes would change a session that it no
    // longer shows, behind the back of the window that does. A window that
    // went before its call came (it closed or detached while ssh signed in)
    // would start a pane that is nobody's, in no layout. A window names
    // itself by the serial of its attach answer. The process of the caller
    // would not do: through `aglow-mux proxy`, each call of a window comes
    // from another proxy. A call that names no window is no window's (the
    // panes of a project that start before its window).
    if (request.args.size() <= windowArg) return {};
    const auto window = request.args[windowArg].toInteger();
    if (!window || *window <= 0) return ipcError(request.id, IpcErrorCode::InvalidArg, "a window is a whole number from 1");
    const Attachment* attachment = shown(name);
    if (attachment && std::uint64_t(*window) == attachment->serial()) return {};
    if (!attachment) return ipcError(request.id, Error::NotAttached, "the window that asked no longer shows session '" + name + "'");
    return ipcError(request.id, Error::Attached, "another window shows session '" + name + "'");
}

std::string Daemon::newPane(const IpcRequest& request)
{
    if (request.args.size() < 4 || request.args.size() > 5 || !request.args[1].isString()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg, "newPane takes (protocol, name, launch, size[, window])");
    }
    if (auto error = versionError(request)) return *error;
    const std::string& name = request.args[1].toString();
    const auto session = find(name);
    if (session == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    if (std::string refusal = windowRefusal(request, name, 4); !refusal.empty()) return refusal;
    if ((*session)->panes().size() >= MaxSessionPanes) return tooManyPanes(request, **session);
    std::string error;
    const auto launch = launchFrom(request.args[2], &error);
    if (!launch) return ipcError(request.id, IpcErrorCode::InvalidArg, error);
    const auto size = paneSizeFrom(request.args[3], &error);
    if (!size) return ipcError(request.id, IpcErrorCode::InvalidArg, error);
    // The window that asked binds the pane before its first frame, which
    // the next pass sends: it knows the ID from this answer. The pane's
    // calls wait for that window from the start.
    const Attachment* window = showing(name);
    const std::uint32_t id = nextPaneId();
    if (!(*session)->addPane(id, *launch, *size, paneSocket(), window ? window->serial() : 0, window ? window->eventQueue() : 0, waker(),
                             &error)) {
        return ipcError(request.id, Error::SpawnFailed, "cannot start the program of a pane of session '" + name + "': " + error);
    }
    return ipcReply(request.id.value_or(0), Json::Object{{"pane", std::int64_t(id)}});
}

std::string Daemon::paneJob(const IpcRequest& request, pid_t caller)
{
    std::string refusal;
    const MuxPane* pane = paneOfCall(request, "paneJob takes (name, pane)", refusal);
    return pane ? ipcReply(request.id.value_or(0), pane->foregroundJob(caller)) : refusal;
}

std::string Daemon::breakOut(const IpcRequest& request)
{
    if (request.args.size() < 3 || request.args.size() > 4 || !request.args[1].isString()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg, "breakOut takes (protocol, name, layout[, window])");
    }
    if (auto error = versionError(request)) return *error;
    const std::string& name = request.args[1].toString();
    const auto from = find(name);
    if (from == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    if (std::string refusal = windowRefusal(request, name, 3); !refusal.empty()) return refusal;
    std::string error;
    auto layout = layoutFrom(request.args[2], &error);
    if (!layout) return ipcError(request.id, IpcErrorCode::InvalidArg, error);
    for (const std::uint32_t id : layout->panes()) {
        if (!(*from)->pane(id)) return ipcError(request.id, Error::NoSuchPane, "session '" + name + "' has no pane " + std::to_string(id));
    }
    // breakOutPanes moves the sessions: the session stays where it is, its
    // iterator does not.
    const auto fresh = breakOutPanes(**from, std::move(*layout), &error);
    if (!fresh) return ipcError(request.id, IpcErrorCode::InvalidArg, error);
    return ipcReply(request.id.value_or(0), Json::Object{{"name", *fresh}});
}

std::optional<std::string> Daemon::breakOutPanes(Session& from, Layout layout, std::string* error)
{
    const std::vector<std::uint32_t> ids = layout.panes();
    if (ids.size() >= from.panes().size()) {
        if (error) *error = "these are all the panes of session '" + from.name() + "', and a new session would only give them another name";
        return std::nullopt;
    }
    // Each allocation before the first pane leaves its session: a failure
    // then leaves every pane where it was, and no pane between two
    // sessions, where its program would end. This one first, as it moves
    // the sessions.
    m_sessions.reserve(m_sessions.size() + 1);
    std::optional<std::string> fresh = freeName();
    // The session's environment, umask and terminal values, and the launch of
    // its first pane, whose variables a split of `afterglow ctl` without a
    // launch of its own takes. Its name is its own, and its panes come with
    // their programs running at their own sizes. The values of a window of a
    // session of another host keep the commands of the panes that it moves
    // (PaneValues::keepsCommand): from this session, before they leave it.
    Session::keepCommands(layout, from.layout());
    SessionOptions options = from.options();
    options.name = *fresh;
    options.size = {};
    auto session = std::make_unique<Session>(*fresh, std::move(options), m_fileLimit, m_images);
    session->setSerial(++m_lastSessionSerial);
    std::vector<std::unique_ptr<MuxPane>> panes;
    panes.reserve(ids.size());
    std::vector<std::pair<std::uint32_t, MuxPane*>> removed;
    removed.reserve(ids.size());
    // From here nothing allocates but the window (guarded).
    for (const std::uint32_t id : ids) {
        panes.push_back(from.take(id));
        removed.emplace_back(id, panes.back().get());
    }
    if (Attachment* attachment = shown(from.name())) {
        guarded(*attachment, [&] {
            attachment->panesRemoved(removed);
            return true;
        });
    }
    // The layout names these panes, each once, and no other: the session
    // takes it as it is.
    session->adopt(std::move(panes), std::move(layout));
    m_sessions.push_back(std::move(session));
    return fresh;
}

std::string Daemon::paneSession(const IpcRequest& request)
{
    const auto id = request.args.size() == 1 ? paneIdFrom(request.args[0]) : std::nullopt;
    if (!id) return ipcError(request.id, IpcErrorCode::InvalidArg, "paneSession takes (pane)");
    if (const Session* session = sessionOfPane(*id)) return ipcReply(request.id.value_or(0), session->name());
    return ipcError(request.id, Error::NoSuchPane, "no session has pane " + std::to_string(*id));
}

MuxPane* Daemon::findPane(std::uint32_t id) const
{
    const Session* session = sessionOfPane(id);
    return session ? session->pane(id) : nullptr;
}

std::uint32_t Daemon::nextPaneId()
{
    while (true) {
        const std::uint32_t id = m_nextPane++;
        if (id != 0 && !findPane(id)) return id;
    }
}

void Daemon::applyLate(Attachment& attachment, Session& own)
{
    Attachment::Late late = attachment.takeLate();
    // The user closed these panes before the window heard: their programs
    // end, and the window that shows the session now no longer shows them.
    for (const std::uint32_t id : late.closedPanes) {
        if (!own.closePane(id)) continue;
        if (Attachment* now = shown(own.name())) {
            guarded(*now, [&] {
                now->paneRemoved(id);
                return true;
            });
        }
    }
    // Its last layout is the next window's, unless a window that shows the
    // session got its layout already: that window shows its own, and sends
    // it when it changes. A layout of a session that ended, whose name a new
    // session took, names none of its panes: pane IDs are not used again.
    // Nor when `afterglow ctl` changed the layout since the window finished:
    // the window's layout would undo a change that it never saw.
    const Attachment* now = shown(own.name());
    if (late.layout && (!now || !now->layoutSent()) && own.layoutChanges() == late.layoutChanges) {
        const std::vector<std::uint32_t> named = late.layout->panes();
        if (std::any_of(named.begin(), named.end(), [&own](std::uint32_t id) { return own.pane(id) != nullptr; })) own.setLayout(std::move(*late.layout));
    }
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
        const Attachment* window = showing(session->name());
        for (const auto& pane : session->panes()) {
            try {
                pane->setWindow(window ? window->serial() : 0, window ? window->eventQueue() : 0);
            } catch (const std::exception& e) {
                // As for a window (guarded): the answers to the calls of a
                // window that went, out of memory, must not end the sessions
                // of all the others. The next pass tries again.
                std::fprintf(stderr, "aglow-mux: a pane of session %s failed: %s\n", session->name().c_str(), e.what());
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
    // A failed window is over: its stream closes, so the window hears it,
    // and the session no longer counts as shown. Nothing else would wake the
    // poll for it, nor for its calls and command clients, which the next
    // pass fails.
    if (std::erase_if(m_attachments, [](const auto& attachment) { return attachment->failed(); }) > 0) m_again = true;
}

Attachment* Daemon::shown(const std::string& name) const
{
    for (const auto& attachment : m_attachments) {
        if (attachment->session() == name && !attachment->finishing()) return attachment.get();
    }
    return nullptr;
}

Attachment* Daemon::showing(const std::string& name) const
{
    Attachment* const attachment = shown(name);
    return attachment && attachment->showing() ? attachment : nullptr;
}

std::string Daemon::tooManyPanes(const IpcRequest& request, const Session& session)
{
    // The window reads the channels of at most that many panes, and the
    // daemon takes no layout with more.
    return ipcError(request.id, Error::TooManyPanes,
                    "session '" + session.name() + "' has " + std::to_string(MaxSessionPanes) + " panes, the most that a session can have");
}

std::string Daemon::failed(const IpcRequest& request, const std::string& message)
{
    return ipcError(request.id, IpcErrorCode::CallFailed, message);
}

bool Daemon::guarded(Attachment& attachment, const std::function<bool()>& work)
{
    try {
        return work();
    } catch (const std::exception& e) {
        // As for a call: one window that fails (out of memory for a frame)
        // must not end the sessions of all the others. That window loses
        // its session, which keeps running.
        std::fprintf(stderr, "aglow-mux: the window of session %s failed: %s\n", attachment.session().c_str(), e.what());
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
        std::vector<std::unique_ptr<MuxPane>> closed;
        if (!contained("the end of a pane", [&] { closed = session.reap(); })) {
            // Out of memory, its panes stay (Session::reap()): the next pass
            // reaps them, and not only at the next wake of a pane. No
            // allocation here (waker() makes a function).
            const std::uint64_t one = 1;
            (void)!::write(m_wakeFd, &one, sizeof one);
            ++it;
            continue;
        }
        for (auto& pane : closed) {
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
        // Its watchers hear the last events of its panes before its end.
        flushWatch(session);
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
