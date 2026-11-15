// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The sockets of `afterglow ssh` in session panes: remote commands, whose
// client runs on the remote host of the ssh session. The ssh session runs in
// a pane of the daemon and outlives any window, and in a session of another
// host it runs there: the daemon keeps the socket that ssh forwards, and the
// answer of the pane to the query of `aglow run` (OSC 24569). It passes each
// connection to the window that shows the session (RelayKind::Remote),
// which serves it as it serves the socket of an ssh session of its own
// (RemoteSockets). The calls and their checks are those of the window's
// control target, in CONTROL.md.

#include "Daemon.h"

#include <AfterglowProto/Random.h>
#include <AfterglowProto/Remote.h>
#include <AfterglowProto/Text.h>
#include <AfterglowProto/Version.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <AfterglowProto/Lock.h>

#include <csignal>

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;
namespace Remote = AfterglowProto::Remote;
using AfterglowProto::ipcError;
using AfterglowProto::ipcReply;
using AfterglowProto::IpcRequest;
using AfterglowProto::IpcServerConnection;
namespace IpcErrorCode = AfterglowProto::IpcErrorCode;

namespace {

std::string failed(const IpcRequest& request, const std::string& message)
{
    return ipcError(request.id, IpcErrorCode::CallFailed, message);
}

/// A path that can go into ssh's -R REMOTE:LOCAL: ssh splits it at ':' and
/// takes a backslash for an escape.
bool forwardable(std::string_view path)
{
    return path.starts_with('/') && path.find(':') == std::string_view::npos && path.find('\\') == std::string_view::npos
        && AfterglowProto::isCleanText(path);
}

/// A whole number of a JSON value, within @p limit either way.
std::optional<std::int64_t> whole(const Json::Value& value, double limit)
{
    if (!value.isNumber()) return std::nullopt;
    const double number = value.toNumber();
    if (!std::isfinite(number) || number != std::floor(number) || std::abs(number) > limit) return std::nullopt;
    return std::int64_t(number);
}

/// The UTF-16 units of @p text, valid UTF-8, as Qt counts a QString.
std::size_t utf16Units(std::string_view text)
{
    std::size_t units = 0;
    char32_t cp = 0;
    for (std::size_t i = 0; i < text.size() && AfterglowProto::nextUtf8(text, i, cp);) units += cp > 0xffff ? 2 : 1;
    return units;
}

int pidfdOpen(pid_t pid)
{
    return int(::syscall(SYS_pidfd_open, pid, 0));
}

} // namespace

Daemon::SshSession::~SshSession()
{
    if (pidfd >= 0) ::close(pidfd);
}

Session* Daemon::sessionOfProcess(pid_t process, std::uint32_t* pane) const
{
    // A pane's program leads the terminal session of the process, or of the
    // nearest process above it (a shell in an editor's terminal has a
    // session of its own). By the processes, not by an AFTERGLOW_PANE_ID
    // that a process of another terminal can carry.
    for (int up = 0; process > 1 && up < MaxAncestors; ++up) {
        const auto stat = AfterglowPty::processStat(process);
        if (!stat) return nullptr;
        for (const auto& s : m_sessions) {
            for (const auto& p : s->panes()) {
                if (!p->leads(pid_t(stat->session))) continue;
                if (pane) *pane = p->id();
                return s.get();
            }
        }
        process = pid_t(stat->parent);
    }
    return nullptr;
}

Session* Daemon::sessionOfPane(std::uint32_t pane) const
{
    for (const auto& s : m_sessions) {
        if (s->pane(pane)) return s.get();
    }
    return nullptr;
}

std::string Daemon::remoteCall(const IpcRequest& request, pid_t caller)
{
    const std::string& fn = request.fn;
    const Json::Array& args = request.args;
    if (fn == "remoteCommandsEnabled") {
        // The settings of the session that the caller runs in, as its last
        // window, else the client that started it, gave them.
        const Session* session = sessionOfProcess(caller, nullptr);
        return ipcReply(request.id.value_or(0), session && session->options().control.remoteCommandsOff.empty());
    }
    if (fn == "remoteSocketForwarded") {
        if (args.size() != 1 || !args[0].isString()) return ipcError(request.id, IpcErrorCode::InvalidArg, "remoteSocketForwarded takes (token)");
        const auto it = m_sshSessions.find(args[0].toString());
        if (it == m_sshSessions.end()) return failed(request, "no remote socket " + args[0].toString() + ": its session has ended");
        SshSession& session = *it->second;
        if (!session.listener) return failed(request, "remote socket " + session.token + " is closed");
        if (!session.answering) {
            session.answering = true;
            refreshAnswer(session.pane);
        }
        return ipcReply(request.id.value_or(0), nullptr);
    }
    if (fn == "remoteSocketFailed") {
        if (args.size() != 2 || !args[0].isString() || !args[1].isString())
            return ipcError(request.id, IpcErrorCode::InvalidArg, "remoteSocketFailed takes (token, reason)");
        const auto it = m_sshSessions.find(args[0].toString());
        if (it == m_sshSessions.end()) return failed(request, "no remote socket " + args[0].toString());
        if (args[1].toString() != "forward") return failed(request, "the reason of a failed forward is forward");
        SshSession& session = *it->second;
        if (!session.listener) return failed(request, "remote socket " + session.token + " has no forward");
        // Nothing to cancel: the forward was never added, or the client
        // cancelled it after its remoteSocketForwarded got no answer in time.
        closeSshSocket(session, "the ssh server refused the forward of the socket, or Afterglow did not confirm it in time");
        session.answer = {{}, Remote::Error::Forward};
        session.answering = true;
        refreshAnswer(session.pane);
        return ipcReply(request.id.value_or(0), nullptr);
    }
    // openRemoteSocket (pane, process, host, directory, protocol, ssh options, destination, overlay).
    if (args.size() != 8 || !args[2].isString() || !args[3].isString() || !args[5].isArray() || !args[6].isString() || !args[7].isBool()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg,
                        "openRemoteSocket takes (pane, process, host, directory, protocol, ssh options, destination, overlay)");
    }
    const auto paneId = whole(args[0], 0xffffffffLL);
    const auto pid = whole(args[1], 1e12);
    const auto protocol = whole(args[4], 1e9);
    if (!paneId || *paneId <= 0) return failed(request, "a pane ID is a whole number from 1");
    if (!pid || !protocol) return failed(request, "the process ID and the protocol are whole numbers");
    Session* const owner = sessionOfPane(std::uint32_t(*paneId));
    if (!owner) return failed(request, "no pane " + std::to_string(*paneId));
    if (!owner->options().control.remoteCommandsOff.empty()) return failed(request, owner->options().control.remoteCommandsOff);
    // A session pane has no overlay: no `afterglow ssh` runs in one.
    if (args[7].toBool()) return failed(request, "a session pane has no overlay");
    const std::string& host = args[2].toString();
    const std::string& directory = args[3].toString();
    const std::string& destination = args[6].toString();
    std::vector<std::string> ssh;
    for (const Json::Value& option : args[5].toArray()) {
        if (!option.isString()) return failed(request, "the ssh options are text");
        ssh.push_back(option.toString());
    }
    if (*pid <= 0 || *pid > 0x7fffffff) return failed(request, "a process ID is a whole number from 1");
    // The pane is the one that the caller and the process it names run in,
    // by their processes: a process of another pane gets no socket that
    // runs remote commands for this one.
    std::uint32_t callerPane = 0;
    std::uint32_t processPane = 0;
    if (!sessionOfProcess(caller, &callerPane) || callerPane != std::uint32_t(*paneId)
        || !sessionOfProcess(pid_t(*pid), &processPane) || processPane != callerPane) {
        return failed(request, "only `afterglow ssh` in pane " + std::to_string(*paneId) + " opens a remote socket for it");
    }
    // ev.remote.host: a name, as `afterglow ssh` shows it.
    if (host.empty() || !AfterglowProto::isCleanText(host) || utf16Units(host) > MaxSshHostLength) {
        return failed(request, "the host is 1 to " + std::to_string(MaxSshHostLength) + " characters of text with no control characters");
    }
    // The path goes into the pane's reply and into ssh's -R.
    if (!forwardable(directory)) {
        return failed(request, "the remote directory must be an absolute path of text with no ':', '\\' and control characters");
    }
    if (ssh.size() > MaxSshOptions) return failed(request, "ssh takes at most " + std::to_string(MaxSshOptions) + " options here");
    if (destination.empty() || destination.starts_with('-')) return failed(request, "the ssh destination must not be empty or start with '-'");
    if (m_sshSessions.size() >= MaxSshSessions) return failed(request, "the mux daemon has " + std::to_string(MaxSshSessions) + " remote sessions already");
    const auto ofPane = std::count_if(m_sshSessions.begin(), m_sshSessions.end(),
                                      [&](const auto& s) { return s.second->pane == std::uint32_t(*paneId); });
    if (std::size_t(ofPane) >= MaxSshSessionsPerPane) {
        return failed(request, "pane " + std::to_string(*paneId) + " has " + std::to_string(MaxSshSessionsPerPane) + " remote sessions already");
    }
    const auto token = AfterglowProto::randomHex(Remote::TokenBytes);
    if (!token) return failed(request, "the kernel gave no random bytes for the token");

    auto session = std::make_unique<SshSession>();
    session->token = *token;
    session->pane = std::uint32_t(*paneId);
    session->order = ++m_sshOpened;
    session->ssh = std::move(ssh);
    session->destination = destination;
    session->host = host;
    session->pidfd = pidfdOpen(pid_t(*pid));
    if (session->pidfd < 0) return failed(request, "no process " + std::to_string(*pid) + ": " + std::strerror(errno));
    session->path = Remote::socketPath(directory, *token);
    if (*protocol != AfterglowProto::ProtocolVersion) {
        session->answer.error = Remote::Error::Version;
        session->answering = true;
    } else if (session->path.size() > Remote::MaxPathBytes) {
        session->answer.error = Remote::Error::Length;
        session->answering = true;
    } else {
        // Beside the daemon's own socket, in the directory that only the user enters.
        const std::string& own = paneSocket();
        const std::string local = own.substr(0, own.rfind('/') + 1) + "remote-" + *token + ".sock";
        if (local.size() > AfterglowProto::MaxSocketPathBytes) return failed(request, "the socket path " + local + " is too long");
        if (!forwardable(local)) return failed(request, "the socket directory " + local + " has a ':', a '\\' or a control character");
        auto listener = std::make_unique<AfterglowProto::IpcListener>();
        std::string error;
        if (listener->listen(local, &error) != AfterglowProto::IpcListener::Listen::Listening) {
            return failed(request, "cannot listen on " + local + (error.empty() ? std::string() : ": " + error));
        }
        session->listener = std::move(listener);
        session->socket = local;
        session->answer.path = session->path;
    }
    Json::Object result{{"token", session->token}};
    if (session->listener) {
        result.emplace_back("socket", session->socket);
        result.emplace_back("path", session->path);
    } else {
        result.emplace_back("error", std::string(Remote::errorName(session->answer.error)));
    }
    const std::uint32_t pane = session->pane;
    const bool answering = session->answering;
    m_sshSessions.emplace(session->token, std::move(session));
    if (answering) refreshAnswer(pane);
    return ipcReply(request.id.value_or(0), std::move(result));
}

void Daemon::refreshAnswer(std::uint32_t pane)
{
    Session* const session = sessionOfPane(pane);
    MuxPane* const target = session ? session->pane(pane) : nullptr;
    if (!target) return;
    // The newest of the pane's sessions that answers: an ssh session that
    // ends gives the answer back to the one before it.
    const SshSession* newest = nullptr;
    for (const auto& [token, s] : m_sshSessions) {
        if (s->pane == pane && s->answering && (!newest || s->order > newest->order)) newest = s.get();
    }
    target->setRemoteSocket(newest ? newest->answer : Remote::Answer{});
}

void Daemon::closeSshSocket(SshSession& session, const std::string& reason)
{
    if (!session.listener) return;
    session.listener->close();
    session.listener.reset();
    // Its clients' runs end with their connections: they hear why.
    std::vector<std::uint64_t> ended;
    for (const auto& [id, relay] : m_relays) {
        if (relay.sshToken == session.token) ended.push_back(id);
    }
    for (const std::uint64_t id : ended) {
        m_relays.at(id).connection->send(ipcError(std::nullopt, IpcErrorCode::CallFailed, reason));
        endRelay(id, true);
    }
}

void Daemon::endSsh(const std::string& token, const std::string& reason)
{
    const auto it = m_sshSessions.find(token);
    if (it == m_sshSessions.end()) return;
    std::unique_ptr<SshSession> session = std::move(it->second);
    m_sshSessions.erase(it);
    if (session->listener) cancelForward(*session);
    closeSshSocket(*session, reason);
    refreshAnswer(session->pane);
}

void Daemon::cancelForward(const SshSession& session)
{
    // Without waiting: when the shared connection has gone, there is
    // nothing to cancel, and the file on the remote host stays until the
    // next probe removes it.
    std::vector<std::string> words{"ssh"};
    words.insert(words.end(), session.ssh.begin(), session.ssh.end());
    for (const std::string& word : {std::string("-O"), std::string("cancel"), std::string("-R"), session.path + ":" + session.socket,
                                    std::string("--"), session.destination}) {
        words.push_back(word);
    }
    std::vector<char*> argv;
    for (std::string& word : words) argv.push_back(word.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd : {0, 1, 2}) posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
    // The daemon blocks the signals of its signalfd and ignores SIGPIPE:
    // ssh gets neither, so that a signal still ends one that hangs.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t none;
    ::sigemptyset(&none);
    sigset_t defaults;
    ::sigemptyset(&defaults);
    for (const int signal : {SIGPIPE, SIGTERM, SIGINT, SIGHUP}) ::sigaddset(&defaults, signal);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        std::fprintf(stderr, "aglow-mux: cannot run ssh to cancel a remote forward: %s\n", std::strerror(spawned));
        return;
    }
    // Reaped when its pidfd says that it ended (handleSshFds). Without one
    // (no descriptor left), each pass of the loop looks: the daemon never
    // waits for ssh.
    m_cancels.push_back({pid, pidfdOpen(pid)});
}

void Daemon::addSshFds(std::vector<pollfd>& fds, std::vector<SshWatch>& watches) const
{
    watches.clear();
    for (const auto& [token, session] : m_sshSessions) {
        if (session->listener) {
            fds.push_back({session->listener->starved() ? -1 : session->listener->fd(), POLLIN, 0});
            watches.push_back({SshWatch::Kind::Listener, token, 0});
        }
        fds.push_back({session->pidfd, POLLIN, 0});
        watches.push_back({SshWatch::Kind::Process, token, 0});
    }
    for (const Cancel& cancel : m_cancels) {
        // A negative descriptor: poll skips it, and handleSshFds looks.
        fds.push_back({cancel.pidfd, POLLIN, 0});
        watches.push_back({SshWatch::Kind::Cancel, {}, cancel.pid});
    }
}

void Daemon::handleSshFds(const pollfd* fds, const std::vector<SshWatch>& watches)
{
    for (std::size_t i = 0; i < watches.size(); ++i) {
        const SshWatch& watch = watches[i];
        if (watch.kind == SshWatch::Kind::Cancel) {
            if (fds[i].fd >= 0 && !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int status = 0;
            if (::waitpid(watch.pid, &status, WNOHANG) == 0) continue;
            const auto it = std::find_if(m_cancels.begin(), m_cancels.end(), [&](const Cancel& c) { return c.pid == watch.pid; });
            if (it != m_cancels.end()) {
                if (it->pidfd >= 0) ::close(it->pidfd);
                m_cancels.erase(it);
            }
            continue;
        }
        if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        const auto it = m_sshSessions.find(watch.token);
        if (it == m_sshSessions.end()) continue;
        if (watch.kind == SshWatch::Kind::Process) {
            endSsh(watch.token, "the ssh session of the remote command ended");
            continue;
        }
    }
    acceptSshClients();
}

void Daemon::acceptSshClients()
{
    // Every socket, also one that the poll did not see: a starved listener
    // is not polled, and only an accept() takes it back.
    for (const auto& [token, session] : m_sshSessions) {
        while (session->listener) {
            const int fd = session->listener->accept();
            if (fd < 0) break;
            acceptSshClient(*session, fd);
        }
    }
}

void Daemon::removeStaleSshSockets(const std::string& directory)
{
    DIR* dir = ::opendir(directory.c_str());
    if (!dir) return;
    std::vector<std::string> sockets;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string_view name = entry->d_name;
        if (name.starts_with("remote-") && name.ends_with(".sock")) sockets.emplace_back(name);
    }
    ::closedir(dir);
    for (const std::string& name : sockets) {
        const std::string path = directory + "/" + name;
        std::string error;
        const int lock = AfterglowProto::lockFile(path + ".lock", &error);
        if (lock < 0) continue;
        ::unlink(path.c_str());
        AfterglowProto::unlockFile(path + ".lock", lock);
    }
}

void Daemon::acceptSshClient(SshSession& session, int fd)
{
    auto connection = std::make_unique<IpcServerConnection>(fd);
    const auto clients = std::count_if(m_relays.begin(), m_relays.end(), [&](const auto& r) { return r.second.sshToken == session.token; });
    if (std::size_t(clients) >= MaxSshConnections) {
        // Before it reads a request, as the socket of a window's ssh session.
        if (!std::exchange(session.full, true)) {
            std::fprintf(stderr, "aglow-mux: the remote socket of the ssh session to %s has %zu connections, and refuses more\n",
                         session.host.c_str(), MaxSshConnections);
        }
        connection->close();
        return;
    }
    Session* const owner = sessionOfPane(session.pane);
    Attachment* const window = owner ? shown(owner->name()) : nullptr;
    if (!window || !window->showing()) {
        // An error that answers no call: the client's first call reads it.
        connection->send(ipcError(std::nullopt, IpcErrorCode::CallFailed,
                                  owner ? "no window shows session '" + owner->name() + "', and only a window runs the commands of scripts"
                                        : std::string("the pane of the ssh session has closed")));
        m_closing.insert(connection.get());
        m_connections.push_back(std::move(connection));
        return;
    }
    const std::uint64_t id = m_nextRelay++;
    const bool sent = guarded(*window, [&] {
        window->sendRelay(Attach::RelayFrame{id, Attach::RelayPart::Open, {}, session.pane, Attach::RelayKind::Remote, session.host});
        return true;
    });
    if (!sent) return;
    m_relays[id] = Relay{connection.get(), window->serial(), owner->name(), {}, session.token};
    m_relayOf[connection.get()] = id;
    m_connections.push_back(std::move(connection));
}

void Daemon::pruneSsh()
{
    // The ssh sessions of panes that closed end: their runs and forwards go.
    // So do those of a session whose window turned remote commands off, as
    // in a window of its own.
    std::vector<std::pair<std::string, std::string>> gone;
    for (const auto& [token, session] : m_sshSessions) {
        const Session* owner = sessionOfPane(session->pane);
        if (!owner) gone.emplace_back(token, "the pane of the ssh session closed");
        else if (!owner->options().control.remoteCommandsOff.empty()) gone.emplace_back(token, owner->options().control.remoteCommandsOff);
    }
    for (const auto& [token, reason] : gone) endSsh(token, reason);
}

void Daemon::endAllSsh()
{
    std::vector<std::string> all;
    for (const auto& [token, session] : m_sshSessions) all.push_back(token);
    for (const std::string& token : all) endSsh(token, "the mux daemon ended");
    // The cancels that run end without the daemon.
    for (const Cancel& cancel : m_cancels) ::close(cancel.pidfd);
    m_cancels.clear();
}

} // namespace AfterglowMux
