// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxConnect.h"

#include "MuxProtocol.h"

#include <AfterglowProto/Text.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string_view>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace AfterglowMux {

using AfterglowProto::IpcConnection;

namespace {

/// The most of a proxy's standard error that a client keeps: enough for a
/// message, and a chatty command does not grow the client.
constexpr std::size_t MaxErrorBytes = 4096;

/// @p text safe to print, line by line: printable() turns a newline into
/// '?' too. Text that another host wrote.
std::string printableLines(std::string_view text)
{
    std::string out;
    bool first = true;
    while (true) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (!first)
            out += '\n';
        first = false;
        out += AfterglowProto::printable(line);
        if (end == std::string_view::npos)
            return out;
        text.remove_prefix(end + 1);
    }
}

/// The answer to the version call on a connection.
struct Version
{
    /// Empty when the daemon speaks this protocol, else why not, safe to
    /// print.
    std::string failure;
    /// The protocol of a daemon that speaks another one, else empty.
    std::string other;
    /// The connection ended without an answer.
    bool ended = false;
    /// No answer came in time.
    bool timedOut = false;
    /// The other end wrote a line that is not JSON: not aglow-mux.
    bool notJson = false;
};

Version checkVersion(IpcConnection& connection, int timeoutMs)
{
    Version version;
    IpcConnection::Error callError;
    const auto answer = connection.call(AfterglowMux::Target, AfterglowMux::Fn::Version, {}, timeoutMs, &callError);
    if (!answer) {
        version.ended = callError.code.empty() && !callError.timedOut;
        version.timedOut = callError.timedOut;
        version.notJson = callError.notJson;
        version.failure = callError.message.empty() ? std::string("no answer") : printableLines(callError.message);
        return version;
    }
    const auto protocol = (*answer)["protocol"].toInteger();
    if (protocol != AfterglowMux::ProtocolVersion) {
        version.other = protocol ? std::to_string(*protocol) : std::string("?");
        // `aglow-mux proxy` checks the daemon of its host too: "this
        // program" is it there, and afterglow here.
        version.failure = "aglow-mux speaks protocol " + version.other + ", and this program speaks "
            + std::to_string(AfterglowMux::ProtocolVersion)
            + ": end its sessions with the afterglow that started it, or wait until they end";
    }
    return version;
}

} // namespace

Environment currentEnvironment()
{
    Environment environment;
    for (char** e = environ; *e; ++e)
        environment.emplace_back(*e);
    return environment;
}

std::string findProgram(const std::string& name, const Environment& environment)
{
    if (name.find('/') != std::string::npos)
        return name;
    std::string_view rest = "/usr/bin:/bin";
    for (const std::string& entry : environment) {
        if (entry.starts_with("PATH=")) {
            rest = std::string_view(entry).substr(5);
            break;
        }
    }
    while (true) {
        const std::size_t colon = rest.find(':');
        const std::string_view directory = rest.substr(0, colon);
        // An empty entry is the current directory, which this never runs from.
        if (!directory.empty()) {
            std::string candidate = std::string(directory) + "/" + name;
            struct stat info{};
            if (::stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode)
                && ::access(candidate.c_str(), X_OK) == 0)
                return candidate;
        }
        if (colon == std::string_view::npos)
            return {};
        rest.remove_prefix(colon + 1);
    }
}

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
    if (m_errorFd >= 0)
        ::close(m_errorFd);
}

bool ProxyProcess::drainError()
{
    char buffer[4096];
    while (m_errorFd >= 0 && !m_errorEnded) {
        const ssize_t n = ::read(m_errorFd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (n <= 0) {
            m_errorEnded = true;
            break;
        }
        m_error.append(buffer, std::min(std::size_t(n), MaxErrorBytes - std::min(MaxErrorBytes, m_error.size())));
    }
    return m_errorFd >= 0 && !m_errorEnded;
}

std::string ProxyProcess::errorOutput() const
{
    // Only here: two lines that came in two reads stay two lines.
    std::string_view text = m_error;
    while (text.ends_with('\n') || text.ends_with('\r'))
        text.remove_suffix(1);
    return text.empty() ? std::string() : printableLines(text);
}

int ProxyProcess::wait(int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (m_status == -2) {
        drainError();
        int status = 0;
        const pid_t reaped = ::waitpid(m_pid, &status, WNOHANG);
        if (reaped == m_pid) {
            drainError();
            m_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        if (reaped < 0 && errno != EINTR) {
            m_status = -1;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return m_status;
}

void endInBackground(std::shared_ptr<ProxyProcess> proxy)
{
    if (!proxy)
        return;
    try {
        std::thread([proxy = std::move(proxy)]() mutable {
            proxy.reset();
        }).detach();
    } catch (const std::system_error&) {
        // No thread: the reference goes here, and this thread waits.
    }
}

Reached reach(const Route& route, bool start, int proxyTimeoutMs, std::string* error)
{
    return reach(route, start, proxyTimeoutMs, currentEnvironment(), error);
}

Reached reach(const Route& route, bool start, int proxyTimeoutMs, const Environment& environment, std::string* error)
{
    error->clear();
    if (route.proxy.empty())
        return connectAny(route.sockets, error);
    // Its standard input and output are one end of a socket pair, which the
    // connection takes; its standard error says why it failed. The read end
    // of that pipe does not block, or a read of it would hang the wait.
    int pair[2] = {-1, -1};
    int errors[2] = {-1, -1};
    const auto closeAll = [&] {
        for (int* fd : {&pair[0], &pair[1], &errors[0], &errors[1]}) {
            if (*fd >= 0)
                ::close(*fd);
            *fd = -1;
        }
    };
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0 || ::pipe2(errors, O_CLOEXEC) != 0) {
        *error = std::string("cannot make the pipes of the proxy: ") + std::strerror(errno);
        closeAll();
        return {};
    }
    if (const int flags = ::fcntl(errors[0], F_GETFL);
        flags < 0 || ::fcntl(errors[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        *error = std::string("cannot make the pipes of the proxy: ") + std::strerror(errno);
        closeAll();
        return {};
    }
    std::vector<std::string> words = route.proxy;
    words.emplace_back(ProxyArgument);
    if (start)
        words.emplace_back(ProxyStartOption);
    std::vector<char*> argv;
    for (auto& word : words)
        argv.push_back(word.data());
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (const std::string& entry : environment)
        envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);
    const std::string program = findProgram(words.front(), environment);
    // A failed action would leave the command the terminal of this process.
    posix_spawn_file_actions_t actions;
    int spawned = posix_spawn_file_actions_init(&actions);
    pid_t pid = -1;
    if (spawned == 0) {
        for (const auto& [from, to] : {std::pair{pair[1], 0}, std::pair{pair[1], 1}, std::pair{errors[1], 2}}) {
            if (spawned == 0)
                spawned = posix_spawn_file_actions_adddup2(&actions, from, to);
        }
        if (spawned == 0)
            spawned = program.empty() ? ENOENT
                                      : posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
    }
    ::close(pair[1]);
    ::close(errors[1]);
    pair[1] = errors[1] = -1;
    if (spawned != 0) {
        closeAll();
        *error = "cannot run " + words.front() + ": " + std::strerror(spawned);
        return {};
    }
    auto proxy = std::make_shared<ProxyProcess>(pid, errors[0]);
    auto connection = std::make_unique<IpcConnection>();
    connection->adopt(pair[0]);
    const Version version = checkVersion(*connection, proxyTimeoutMs);
    if (version.failure.empty())
        return {std::move(proxy), std::move(connection)};
    connection->close();
    const int status = proxy->wait(version.ended ? 5000 : 0);
    const std::string said = proxy->errorOutput();
    if (status == 127) {
        // The shell there, or the loader of a program that lacks a library.
        // A shell over ssh says "not found" in its own words: the hint stays
        // with them.
        const std::string& program = route.proxy.back();
        const std::string named = program == "aglow-mux" ? program : "aglow-mux (" + program + ")";
        *error = said.empty() ? route.host + " has no " + named + ": install it there"
                              : route.host + " cannot run " + named + ": " + said + ": install it there";
    } else if (version.notJson) {
        *error = route.host + " wrote text that is not from aglow-mux: a login file of the shell there, such as .bashrc, .zshenv or config.fish, "
                 "writes on standard output when the shell is not interactive";
    } else if (status == ProxyNoDaemon) {
        return {};
    } else if (version.ended && !said.empty()) {
        *error = said;
    } else if (!version.other.empty()) {
        // The proxy there checked its daemon: both are another build.
        *error = "the aglow-mux of " + route.host + " speaks protocol " + version.other + ", and this afterglow speaks "
            + std::to_string(AfterglowMux::ProtocolVersion) + ": use the same version of Afterglow on both computers";
    } else {
        *error = version.failure;
    }
    return {};
}

Reached connectAny(const AfterglowProto::MuxSockets& sockets, std::string* error, int timeoutMs)
{
    error->clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
    // The wait for one step: @p most, or what is left of timeoutMs.
    const auto stepWait = [&](int most) {
        if (timeoutMs < 0)
            return most;
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        return int(std::clamp<long long>(left, 0, most));
    };
    for (const AfterglowProto::MuxSocket& socket : sockets.candidates) {
        const int connectWait = stepWait(ConnectTimeoutMs);
        if (connectWait == 0)
            break;
        auto connection = std::make_unique<IpcConnection>();
        IpcConnection::Error connectError;
        if (!connection->connect(socket.path, &connectError, connectWait)) {
            // A wait that the limit cut short tells nothing.
            const bool cut = connectError.timedOut && connectWait < ConnectTimeoutMs;
            if (!connectError.noServer && !cut && error->empty())
                *error = printableLines(connectError.message);
            continue;
        }
        const int callWait = stepWait(CallTimeoutMs);
        if (callWait == 0)
            break;
        const Version version = checkVersion(*connection, callWait);
        if (!version.failure.empty()) {
            // A daemon that closed the connection without an answer was
            // ending: as no daemon there. One that did not answer before the
            // limit is not up yet.
            if (version.ended || (version.timedOut && callWait < CallTimeoutMs))
                continue;
            *error = version.failure;
            return {};
        }
        // A first socket that failed does not matter once one answers.
        error->clear();
        return {nullptr, std::move(connection)};
    }
    return {};
}

} // namespace AfterglowMux
