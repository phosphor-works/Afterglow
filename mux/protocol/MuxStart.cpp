// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxStart.h"

#include "MuxProtocol.h"

#include <AfterglowProto/Lock.h>
#include <AfterglowProto/Text.h>
#include <AfterglowPty/CloseFrom.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace AfterglowMux {

namespace {

using AfterglowProto::printable;

// The daemon of a client that just started it listens within this time.
constexpr auto StartWait = std::chrono::seconds(5);
// The unit of a daemon that the systemd user manager runs.
constexpr const char* Unit = "afterglow-mux";

// Whether a daemon holds a socket: it takes the lock before it binds and
// keeps it until it ends.
bool lockHeldElsewhere(const AfterglowProto::MuxSockets& sockets)
{
    return std::any_of(sockets.candidates.cbegin(), sockets.candidates.cend(),
                       [](const AfterglowProto::MuxSocket& socket) { return AfterglowProto::lockFileHeld(socket.path + ".lock"); });
}

std::string userName()
{
    passwd entry{};
    passwd* found = nullptr;
    std::vector<char> buffer(16384);
    if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &found) != 0 || !found) return {};
    return found->pw_name;
}

/// Whether the systemd user manager runs the daemon: it has its own unit
/// there, so systemd-oomd stopping the unit of a window leaves the sessions.
/// Only for the user's own runtime directory: a daemon of another one (a
/// test's) must not take the unit name of the user's daemon. And only when
/// the manager outlives the logins that it serves: with lingering on, or
/// from a graphical login, which a user ends on purpose. The manager of an
/// SSH login without lingering stops its units at the logout, which would end
/// the sessions.
bool userManager()
{
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || std::string(runtime) != "/run/user/" + std::to_string(::getuid())) return false;
    struct stat info{};
    if (::stat((std::string(runtime) + "/systemd/private").c_str(), &info) != 0 || !S_ISSOCK(info.st_mode)) return false;
    if (const std::string user = userName(); !user.empty() && ::access(("/var/lib/systemd/linger/" + user).c_str(), F_OK) == 0) {
        return true;
    }
    const char* session = std::getenv("XDG_SESSION_TYPE");
    return session && (std::strcmp(session, "wayland") == 0 || std::strcmp(session, "x11") == 0);
}

/// Runs @p argv with its standard streams at /dev/null, or its standard
/// output in @p output, and waits for it. Its exit status, or -1.
int run(const std::vector<std::string>& argv, std::string* output = nullptr)
{
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    int pipe[2] = {-1, -1};
    if (output && ::pipe2(pipe, O_CLOEXEC) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd : {0, 1, 2}) posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
    if (output) posix_spawn_file_actions_adddup2(&actions, pipe[1], 1);
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (output) {
        ::close(pipe[1]);
        char buffer[4096];
        for (ssize_t n; spawned == 0 && (n = ::read(pipe[0], buffer, sizeof buffer)) != 0;) {
            if (n > 0) output->append(buffer, std::size_t(n));
            else if (errno != EINTR) break;
        }
        ::close(pipe[0]);
    }
    if (spawned != 0) return -1;
    // The caller set SIGCHLD to its default (connectOrStart), so the status is the child's. A
    // waitpid() that fails tells nothing: unknown, not 0.
    int status = 0;
    pid_t reaped;
    while ((reaped = ::waitpid(pid, &status, 0)) < 0 && errno == EINTR) { }
    if (reaped != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/// The value of @p property of the daemon's unit in the user manager, or
/// empty when systemctl cannot say.
std::string unitProperty(const char* property)
{
    std::string value;
    if (run({"systemctl", "--user", "show", std::string("--property=") + property, "--value", std::string(Unit) + ".service"}, &value) != 0) {
        return {};
    }
    while (value.ends_with('\n')) value.pop_back();
    return value;
}

/// Whether the user manager has the daemon's unit, in any state: then
/// systemd-run fails for its name, and the daemon of that unit starts or
/// ends.
bool unitLoaded()
{
    return unitProperty("LoadState") == "loaded";
}

/// Starts @p daemon in a session of its own, outside the process group and
/// the terminal of this command: it stays when they end. Its standard
/// streams go to /dev/null, so it reports on @p reportFd
/// (AfterglowMux::ReportFdVariable) that it listens, or why it cannot start.
bool startDetached(const std::string& daemon, int reportFd, std::string* error)
{
    // Made before the fork: the child calls only what is safe after one.
    std::vector<std::string> store;
    const std::string reportVariable = std::string(AfterglowMux::ReportFdVariable) + "=";
    for (char** e = environ; *e; ++e) {
        if (!std::string_view(*e).starts_with(reportVariable)) store.emplace_back(*e);
    }
    store.emplace_back(reportVariable + "3");
    std::vector<char*> envp;
    for (auto& entry : store) envp.push_back(entry.data());
    envp.push_back(nullptr);
    char* const args[] = {const_cast<char*>(daemon.c_str()), nullptr};
    const std::string execFailed = "afterglow-mux: cannot run " + daemon + "\n";

    const pid_t child = ::fork();
    if (child < 0) {
        *error = std::string("cannot fork: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        ::setsid();
        // A second fork: the daemon is no session leader, so it can never
        // take a controlling terminal.
        const pid_t daemonPid = ::fork();
        if (daemonPid < 0) ::_exit(127);
        if (daemonPid > 0) ::_exit(0);
        // Descriptor 3 is the report pipe, and the daemon gets no other
        // descriptor of this command: not the socket of a window, nor a pipe
        // whose reader waits for its end.
        if (reportFd != 3) ::dup2(reportFd, 3);
        else ::fcntl(3, F_SETFD, 0);
        AfterglowPty::closeFrom(4);
        const int null = ::open("/dev/null", O_RDWR);
        for (int fd = 0; fd < 3; ++fd) {
            if (fd != null) ::dup2(null, fd);
        }
        if (null > 3) ::close(null);
        (void)!::chdir("/");
        ::execve(daemon.c_str(), args, envp.data());
        (void)!::write(3, execFailed.data(), execFailed.size());
        ::_exit(127);
    }
    int status = 0;
    pid_t reaped;
    while ((reaped = ::waitpid(child, &status, 0)) < 0 && errno == EINTR) { }
    if (reaped != child) {
        *error = std::string("cannot start afterglow-mux: cannot wait for its starter: ") + std::strerror(errno);
        return false;
    }
    if (WIFSIGNALED(status)) {
        *error = "cannot start afterglow-mux: its starter was killed by signal " + std::to_string(WTERMSIG(status));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        *error = "cannot start afterglow-mux: the second fork failed";
        return false;
    }
    return true;
}

/// What the daemon reported on its pipe.
struct Report
{
    enum class State {
        Listening,
        Failed, ///< message says why
        Held, ///< another daemon holds a socket, which message names
        Ended, ///< the pipe ended with no report: the daemon ended before it listened
        TimedOut, ///< nothing before the deadline
    };
    State state = State::TimedOut;
    std::string message;
};

/// Reads the report of the daemon on @p fd, up to @p deadline.
Report readReport(int fd, std::chrono::steady_clock::time_point deadline)
{
    Report report;
    bool ended = false;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        pollfd p{fd, POLLIN, 0};
        const int ready = ::poll(&p, 1, int(left.count()));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        char buffer[4096];
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n > 0) {
            report.message.append(buffer, std::size_t(n));
            if (report.message.size() == 1 && report.message.front() == AfterglowMux::ReportListening) {
                return {Report::State::Listening, {}};
            }
            continue;
        }
        ended = true;
        break;
    }
    if (report.message.empty()) {
        if (ended) report.state = Report::State::Ended;
    } else if (report.message.front() == AfterglowMux::ReportHeld) {
        report.state = Report::State::Held;
        report.message.erase(0, 1);
    } else {
        report.state = Report::State::Failed;
    }
    return report;
}

/// The paths of @p sockets, for a message.
std::string paths(const AfterglowProto::MuxSockets& sockets)
{
    std::string list;
    for (const auto& socket : sockets.candidates) list += (list.empty() ? "" : " and ") + socket.path;
    return list;
}

/// Waits until a daemon answers on one of @p sockets, up to @p deadline.
Reached waitForDaemon(const AfterglowProto::MuxSockets& sockets, std::chrono::steady_clock::time_point deadline,
                      std::string* error)
{
    while (true) {
        Reached reached = connectAny(sockets, error);
        if (reached.connection || !error->empty() || std::chrono::steady_clock::now() >= deadline) return reached;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace

Reached connectOrStart(const AfterglowProto::MuxSockets& sockets, const std::string& daemon, std::string* error)
{
    if (Reached reached = connectAny(sockets, error); reached.connection || !error->empty()) return reached;
    const auto deadline = std::chrono::steady_clock::now() + StartWait;
    if (userManager()) {
        std::vector<std::string> start = {"systemd-run", "--user", "--quiet", "--collect",
                                          std::string("--unit=") + Unit, "--description=Afterglow terminal sessions"};
        // The daemon runs in the user manager's environment, and finds its
        // state directory's socket from these: the one that this command and
        // later clients look for.
        for (const char* name : {"XDG_STATE_HOME", "HOME"}) {
            if (const char* value = std::getenv(name); value && *value) start.push_back(std::string("--setenv=") + name + "=" + value);
        }
        start.push_back(daemon);
        // systemd-run fails too when the unit exists: another command
        // started the daemon at the same time, or the daemon of the unit
        // ends. Then a second daemon would run outside the user manager.
        bool ours = run(start) == 0;
        if (ours || unitLoaded()) {
            // A unit that ends keeps its name until its daemon is gone, and
            // then --collect unloads it. Nothing starts a daemon after that,
            // so this command starts it once more. A unit that this command
            // started and that is gone had a daemon that ended: nothing
            // answers, unless that daemon found a socket held by one that
            // another command started outside the user manager. The holder
            // keeps its lock file from before it binds until it ends.
            bool retried = false;
            // Each look at the unit runs systemctl: a few times a second,
            // while the connects try every 20 ms.
            auto nextLook = std::chrono::steady_clock::now();
            while (true) {
                Reached reached = connectAny(sockets, error);
                if (reached.connection || !error->empty()) return reached;
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) break;
                if (now >= nextLook) {
                    nextLook = now + std::chrono::milliseconds(250);
                    if (!unitLoaded()) {
                        if (!ours && !retried) {
                            retried = true;
                            ours = run(start) == 0;
                        } else if (!lockHeldElsewhere(sockets)) {
                            break;
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            // The daemon of a unit that this command started and that still
            // runs holds its own locks: its error is in the journal.
            if (!(ours && unitLoaded()) && lockHeldElsewhere(sockets)) {
                *error = "afterglow-mux did not start: another afterglow-mux holds a socket and does not answer";
            } else if (unitProperty("ActiveState") == "deactivating") {
                *error = std::string("afterglow-mux did not start: the daemon of ") + Unit
                    + ".service is ending. Try again when it has ended";
            } else {
                *error = std::string("afterglow-mux did not start: see journalctl --user -u ") + Unit;
            }
            return {};
        }
    }
    int report[2] = {-1, -1};
    if (::pipe2(report, O_CLOEXEC) != 0) {
        *error = std::string("cannot create a pipe: ") + std::strerror(errno);
        return {};
    }
    const bool started = startDetached(daemon, report[1], error);
    ::close(report[1]);
    Report reported;
    if (started) reported = readReport(report[0], deadline);
    ::close(report[0]);
    if (!started) return {};
    // A daemon that found another one holding a socket (another command
    // started it at the same time) leaves that one to answer: it took its
    // lock before it binds, so the wait is the same as for this one. Any
    // other failure: one look, no wait.
    const bool waits = reported.state == Report::State::Listening || reported.state == Report::State::Held;
    Reached reached = waitForDaemon(sockets, waits ? deadline : std::chrono::steady_clock::now(), error);
    if (!reached.connection && error->empty()) {
        std::string& reason = reported.message;
        while (reason.ends_with('\n')) reason.pop_back();
        switch (reported.state) {
        case Report::State::Failed:
            *error = printable(reason);
            break;
        case Report::State::Held:
            *error = printable(reason) + ", which does not answer";
            break;
        case Report::State::Ended:
            *error = "afterglow-mux ended before it listened";
            break;
        case Report::State::Listening:
        case Report::State::TimedOut:
            *error = "afterglow-mux did not start: no daemon answers on " + paths(sockets);
            break;
        }
    }
    return reached;
}


} // namespace AfterglowMux
