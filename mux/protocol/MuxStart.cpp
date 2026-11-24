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
#include <csignal>
#include <cstring>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace AfterglowMux {

namespace {

using AfterglowProto::printable;

// How long a look at the unit after the start wait may take, for the message.
constexpr auto LookWait = std::chrono::seconds(1);
// The unit of a daemon that the systemd user manager runs.
constexpr const char* Unit = "aglow-mux";

// Whether a daemon holds a socket: it takes the lock before it binds and
// keeps it until it ends.
bool lockHeldElsewhere(const AfterglowProto::MuxSockets& sockets)
{
    return std::any_of(sockets.candidates.cbegin(), sockets.candidates.cend(),
                       [](const AfterglowProto::MuxSocket& socket) {
                           return AfterglowProto::lockFileHeld(socket.path + ".lock");
                       });
}

/// The value of @p name in @p environment, or nullptr.
const char* lookup(const Environment& environment, std::string_view name)
{
    for (const std::string& entry : environment) {
        if (entry.size() > name.size() && entry.starts_with(name) && entry[name.size()] == '=')
            return entry.c_str() + name.size() + 1;
    }
    return nullptr;
}

/// The state directory of @p environment, as muxSockets() finds it, or
/// empty: XDG_STATE_HOME, else ~/.local/state, each an absolute path.
std::string stateDirectory(const Environment& environment)
{
    const auto trimmed = [](std::string path) {
        while (path.size() > 1 && path.ends_with('/'))
            path.pop_back();
        return path;
    };
    if (const char* state = lookup(environment, "XDG_STATE_HOME"); state && *state == '/')
        return trimmed(state);
    if (const char* home = lookup(environment, "HOME"); home && *home == '/') {
        std::string directory = trimmed(home);
        if (!directory.ends_with('/'))
            directory += '/';
        return directory + ".local/state";
    }
    return {};
}

std::string userName()
{
    passwd entry{};
    passwd* found = nullptr;
    std::vector<char> buffer(16384);
    if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &found) != 0 || !found)
        return {};
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
bool userManager(const Environment& environment)
{
    const char* runtime = lookup(environment, "XDG_RUNTIME_DIR");
    if (!runtime || std::string(runtime) != "/run/user/" + std::to_string(::getuid()))
        return false;
    struct stat info{};
    if (::stat((std::string(runtime) + "/systemd/private").c_str(), &info) != 0 || !S_ISSOCK(info.st_mode))
        return false;
    if (const std::string user = userName();
        !user.empty() && ::access(("/var/lib/systemd/linger/" + user).c_str(), F_OK) == 0) {
        return true;
    }
    const char* session = lookup(environment, "XDG_SESSION_TYPE");
    return session && (std::strcmp(session, "wayland") == 0 || std::strcmp(session, "x11") == 0);
}

/// The milliseconds until @p deadline, at least 0.
int msUntil(std::chrono::steady_clock::time_point deadline)
{
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return int(std::clamp<long long>(left, 0, 1 << 30));
}

/// Runs @p argv in @p environment with its standard streams at /dev/null,
/// or its standard output in @p output, and waits for it up to
/// @p deadline, when it is killed. Its exit status, or as
/// UserManager::run says.
int run(const std::vector<std::string>& argv, const Environment& environment,
        std::chrono::steady_clock::time_point deadline, std::string* output = nullptr)
{
    const std::string program = findProgram(argv.front(), environment);
    if (program.empty())
        return -1;
    std::vector<char*> args;
    for (const auto& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    std::vector<char*> envp;
    for (const auto& entry : environment)
        envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);
    int pipe[2] = {-1, -1};
    if (output && ::pipe2(pipe, O_CLOEXEC) != 0)
        return -1;
    posix_spawn_file_actions_t actions;
    int spawned = posix_spawn_file_actions_init(&actions);
    pid_t pid = 0;
    if (spawned == 0) {
        // A failed action would leave the program the streams of this process.
        for (int fd : {0, 1, 2}) {
            if (spawned == 0)
                spawned = posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
        }
        if (spawned == 0 && output)
            spawned = posix_spawn_file_actions_adddup2(&actions, pipe[1], 1);
        if (spawned == 0)
            spawned = posix_spawn(&pid, program.c_str(), &actions, nullptr, args.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
    }
    if (output) {
        ::close(pipe[1]);
        char buffer[4096];
        while (spawned == 0) {
            pollfd p{pipe[0], POLLIN, 0};
            const int ready = ::poll(&p, 1, msUntil(deadline));
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready <= 0)
                break;
            const ssize_t n = ::read(pipe[0], buffer, sizeof buffer);
            if (n > 0)
                output->append(buffer, std::size_t(n));
            else if (n == 0 || errno != EINTR)
                break;
        }
        ::close(pipe[0]);
    }
    if (spawned != 0)
        return -1;
    // The caller of connectOrStart left SIGCHLD at its default, so the
    // status is the child's. A waitpid() that fails tells nothing: unknown,
    // not 0.
    while (true) {
        int status = 0;
        const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (reaped < 0 && errno != EINTR)
            return -1;
        // A user manager that does not answer holds the start no longer.
        // Not -1: a program that did not answer says nothing about the unit.
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) { }
            return UserManager::TimedOut;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

/// Why the start stops when systemd-run or systemctl did not end by the
/// deadline: whether the unit exists is not known, and a daemon started
/// outside the user manager could be a second one.
constexpr const char* NoAnswer = "aglow-mux did not start: the systemd user manager does not answer";

/// The value of @p property of the daemon's unit in the user manager: empty
/// when systemctl fails, nothing when it did not end by @p deadline.
std::optional<std::string> unitProperty(const char* property, const UserManager& manager,
                                        std::chrono::steady_clock::time_point deadline)
{
    std::string value;
    const int status = manager.run({"systemctl", "--user", "show", std::string("--property=") + property, "--value",
                                    std::string(Unit) + ".service"},
                                   deadline, &value);
    if (status == UserManager::TimedOut)
        return std::nullopt;
    if (status != 0)
        return std::string();
    while (value.ends_with('\n'))
        value.pop_back();
    return value;
}

/// Whether the user manager has the daemon's unit, in any state: then
/// systemd-run fails for its name, and the daemon of that unit starts or
/// ends. Nothing when systemctl did not answer.
std::optional<bool> unitLoaded(const UserManager& manager, std::chrono::steady_clock::time_point deadline)
{
    const std::optional<std::string> state = unitProperty("LoadState", manager, deadline);
    if (!state)
        return std::nullopt;
    return *state == "loaded";
}

/// Whether the daemon's unit is one that the user wrote (a unit file), not
/// one that systemd-run made, and does not run: nothing starts it, and
/// systemd-run cannot take its name. Nothing when systemctl did not answer.
std::optional<bool> ownUnitStopped(const UserManager& manager, std::chrono::steady_clock::time_point deadline)
{
    // systemctl says "no" and "inactive" for a unit that it does not have too.
    const std::optional<bool> loaded = unitLoaded(manager, deadline);
    if (!loaded || !*loaded)
        return loaded;
    const std::optional<std::string> transient = unitProperty("Transient", manager, deadline);
    if (!transient)
        return std::nullopt;
    if (*transient != "no")
        return false;
    const std::optional<std::string> state = unitProperty("ActiveState", manager, deadline);
    if (!state)
        return std::nullopt;
    return *state == "inactive" || *state == "failed";
}

/// What the starter of the daemon writes on the report pipe without
/// /dev/null, before the errno digits.
constexpr const char* NoNullFailure = "aglow-mux: cannot open /dev/null: errno ";

/// The text that the starter of the daemon writes on the report pipe when
/// it cannot run @p daemon, before the errno digits.
std::string execFailure(const std::string& daemon)
{
    return "aglow-mux: cannot run " + daemon + ": errno ";
}

/// Writes @p prefix and the digits of @p number on @p fd: only what is safe
/// after a fork.
void writeFailure(int fd, const std::string& prefix, int number)
{
    char digits[16];
    std::size_t at = sizeof digits;
    unsigned value = number < 0 ? 0u : unsigned(number);
    do {
        digits[--at] = char('0' + value % 10);
        value /= 10;
    } while (value != 0 && at > 0);
    (void)!::write(fd, prefix.data(), prefix.size());
    (void)!::write(fd, digits + at, sizeof digits - at);
}

/// Starts @p daemon in a session of its own, outside the process group and
/// the terminal of this command: it stays when they end. Its standard
/// streams go to /dev/null, so it reports on @p reportFd
/// (AfterglowMux::ReportFdVariable) that it listens, or why it cannot start.
bool startDetached(const std::string& daemon, const Environment& environment, int reportFd, std::string* error)
{
    // Made before the fork: the child calls only what is safe after one.
    std::vector<std::string> store;
    const std::string reportVariable = std::string(AfterglowMux::ReportFdVariable) + "=";
    for (const std::string& entry : environment) {
        if (!entry.starts_with(reportVariable))
            store.push_back(entry);
    }
    store.emplace_back(reportVariable + "3");
    std::vector<char*> envp;
    for (auto& entry : store)
        envp.push_back(entry.data());
    envp.push_back(nullptr);
    char* const args[] = {const_cast<char*>(daemon.c_str()), nullptr};
    const std::string execFailed = execFailure(daemon);
    const std::string noNull = NoNullFailure;

    // No handler of this process may run in the child before it resets
    // them, as Pty::spawn does.
    sigset_t blockAll;
    sigset_t callerMask;
    sigfillset(&blockAll);
    pthread_sigmask(SIG_SETMASK, &blockAll, &callerMask);
    const pid_t child = ::fork();
    const int forkError = errno;
    if (child != 0)
        pthread_sigmask(SIG_SETMASK, &callerMask, nullptr);
    if (child < 0) {
        *error = std::string("cannot fork: ") + std::strerror(forkError);
        return false;
    }
    if (child == 0) {
        // The daemon gets the default actions and an empty mask, not those
        // of the thread that started it. The handlers go first.
        struct sigaction fallback{};
        fallback.sa_handler = SIG_DFL;
        for (int sig = 1; sig < NSIG; ++sig) {
            if (sig != SIGKILL && sig != SIGSTOP)
                ::sigaction(sig, &fallback, nullptr);
        }
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::setsid();
        // A second fork: the daemon is no session leader, so it can never
        // take a controlling terminal.
        const pid_t daemonPid = ::fork();
        if (daemonPid < 0)
            ::_exit(127);
        if (daemonPid > 0)
            ::_exit(0);
        // Descriptor 3 is the report pipe, and the daemon gets no other
        // descriptor of this command: not the socket of a window, nor a pipe
        // whose reader waits for its end.
        if (reportFd != 3)
            ::dup2(reportFd, 3);
        else
            ::fcntl(3, F_SETFD, 0);
        AfterglowPty::closeFrom(4);
        // Without /dev/null the daemon would keep the streams of this
        // command: the ssh channel of a proxy, which then never ends.
        const int null = ::open("/dev/null", O_RDWR);
        if (null < 0) {
            writeFailure(3, noNull, errno);
            ::_exit(127);
        }
        for (int fd = 0; fd < 3; ++fd) {
            if (fd != null)
                ::dup2(null, fd);
        }
        if (null > 3)
            ::close(null);
        (void)!::chdir("/");
        ::execve(daemon.c_str(), args, envp.data());
        writeFailure(3, execFailed, errno);
        ::_exit(127);
    }
    int status = 0;
    pid_t reaped;
    while ((reaped = ::waitpid(child, &status, 0)) < 0 && errno == EINTR) { }
    if (reaped != child) {
        *error = std::string("cannot start aglow-mux: cannot wait for its starter: ") + std::strerror(errno);
        return false;
    }
    if (WIFSIGNALED(status)) {
        *error = "cannot start aglow-mux: its starter was killed by signal " + std::to_string(WTERMSIG(status));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        *error = "cannot start aglow-mux: the second fork failed";
        return false;
    }
    return true;
}

/// @p reason with the errno digits of the starter (writeFailure) as text.
std::string failureText(const std::string& reason, const std::string& daemon)
{
    for (const std::string& prefix : {execFailure(daemon), std::string(NoNullFailure)}) {
        if (!reason.starts_with(prefix))
            continue;
        const std::string_view digits = std::string_view(reason).substr(prefix.size());
        if (digits.empty() || digits.size() > 9 || !std::all_of(digits.begin(), digits.end(), [](char c) {
                return c >= '0' && c <= '9';
            }))
            break;
        return prefix.substr(0, prefix.size() - std::strlen("errno ")) + std::strerror(std::stoi(std::string(digits)));
    }
    return reason;
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
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
            break;
        pollfd p{fd, POLLIN, 0};
        const int ready = ::poll(&p, 1, int(left.count()));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;
        char buffer[4096];
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
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
        if (ended)
            report.state = Report::State::Ended;
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
    for (const auto& socket : sockets.candidates)
        list += (list.empty() ? "" : " and ") + socket.path;
    return list;
}

/// Waits until a daemon answers on one of @p sockets, up to @p deadline.
Reached waitForDaemon(const AfterglowProto::MuxSockets& sockets, std::chrono::steady_clock::time_point deadline,
                      std::string* error)
{
    while (true) {
        // Each look ends by the deadline too.
        Reached reached = connectAny(sockets, error, std::max(msUntil(deadline), 1));
        if (reached.connection || !error->empty() || std::chrono::steady_clock::now() >= deadline)
            return reached;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace

Reached connectOrStart(const AfterglowProto::MuxSockets& sockets, const std::string& daemon, std::string* error)
{
    return connectOrStart(sockets, daemon, currentEnvironment(), error);
}

Reached connectOrStart(const AfterglowProto::MuxSockets& sockets, const std::string& daemon,
                       const Environment& environment, std::string* error, std::chrono::milliseconds wait)
{
    return connectOrStart(sockets, daemon, environment, userManagerOf(environment), error, wait);
}

UserManager userManagerOf(const Environment& environment)
{
    UserManager manager;
    manager.runs = userManager(environment);
    manager.run = [environment](const std::vector<std::string>& argv, std::chrono::steady_clock::time_point deadline,
                                std::string* output) {
        return run(argv, environment, deadline, output);
    };
    return manager;
}

Reached connectOrStart(const AfterglowProto::MuxSockets& sockets, const std::string& daemon,
                       const Environment& environment, const UserManager& manager, std::string* error,
                       std::chrono::milliseconds wait)
{
    if (Reached reached = connectAny(sockets, error); reached.connection || !error->empty())
        return reached;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    if (manager.runs && manager.run) {
        // Before the message that the manager does not answer, one more look
        // at the sockets, which ends by @p look: the daemon that another
        // command started at the same time (systemd-run failed for its
        // unit), or that a start that the manager did not answer brought,
        // may answer by now.
        const auto noAnswer = [&](std::chrono::steady_clock::time_point look) {
            Reached reached = connectAny(sockets, error, std::max(msUntil(look), 1));
            if (!reached.connection && error->empty())
                *error = NoAnswer;
            return reached;
        };
        std::vector<std::string> start = {"systemd-run",
                                          "--user",
                                          "--quiet",
                                          "--collect",
                                          std::string("--unit=") + Unit,
                                          "--description=Afterglow terminal sessions"};
        // The daemon runs in the user manager's environment, and finds its
        // state directory's socket from these: the one that this command and
        // later clients look for. The state directory always, as
        // muxSockets() finds it: the manager's own XDG_STATE_HOME can be
        // another one.
        if (const std::string state = stateDirectory(environment); !state.empty())
            start.push_back("--setenv=XDG_STATE_HOME=" + state);
        if (const char* home = lookup(environment, "HOME"); home && *home)
            start.push_back(std::string("--setenv=HOME=") + home);
        start.push_back(daemon);
        // systemd-run fails too when the unit exists: another command
        // started the daemon at the same time, or the daemon of the unit
        // ends. Then a second daemon would run outside the user manager. So
        // does a start that the manager did not answer in time, which can
        // still bring the unit's daemon.
        const int status = manager.run(start, deadline, nullptr);
        if (status == UserManager::TimedOut)
            return noAnswer(std::chrono::steady_clock::now() + LookWait);
        bool ours = status == 0;
        // A unit file of that name that does not run: no daemon of it comes.
        if (!ours) {
            const std::optional<bool> stopped = ownUnitStopped(manager, deadline);
            if (!stopped)
                return noAnswer(std::chrono::steady_clock::now() + LookWait);
            if (*stopped) {
                *error = std::string("aglow-mux did not start: the user unit ") + Unit
                    + ".service exists and does not run. Start it with systemctl --user start " + Unit
                    + ".service, or remove it";
                return {};
            }
        }
        const std::optional<bool> loaded = ours ? std::optional<bool>(true) : unitLoaded(manager, deadline);
        if (!loaded)
            return noAnswer(std::chrono::steady_clock::now() + LookWait);
        if (*loaded) {
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
                Reached reached = connectAny(sockets, error, std::max(msUntil(deadline), 1));
                if (reached.connection || !error->empty())
                    return reached;
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                    break;
                if (now >= nextLook) {
                    nextLook = now + std::chrono::milliseconds(250);
                    // A look that got no answer tells nothing: the wait goes on.
                    if (const std::optional<bool> still = unitLoaded(manager, deadline); still && !*still) {
                        if (!ours && !retried) {
                            retried = true;
                            const int again = manager.run(start, deadline, nullptr);
                            if (again == UserManager::TimedOut)
                                return noAnswer(std::chrono::steady_clock::now() + LookWait);
                            ours = again == 0;
                        } else if (!lockHeldElsewhere(sockets)) {
                            break;
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            // The daemon of a unit that this command started and that still
            // runs holds its own locks: its error is in the journal. The looks
            // for this message have a short wait of their own. A look that
            // used it up leaves the last look at the sockets a wait of its own.
            const auto look = std::chrono::steady_clock::now() + LookWait;
            const std::optional<bool> runs = ours ? unitLoaded(manager, look) : std::optional<bool>(false);
            if (!runs)
                return noAnswer(std::chrono::steady_clock::now() + LookWait);
            if (!*runs && lockHeldElsewhere(sockets)) {
                *error = "aglow-mux did not start: another aglow-mux holds a socket and does not answer";
            } else if (const std::optional<std::string> state = unitProperty("ActiveState", manager, look); !state) {
                return noAnswer(std::chrono::steady_clock::now() + LookWait);
            } else if (*state == "deactivating") {
                *error = std::string("aglow-mux did not start: the daemon of ") + Unit
                    + ".service is ending. Try again when it has ended";
            } else {
                *error = std::string("aglow-mux did not start: see journalctl --user -u ") + Unit;
            }
            return {};
        }
    }
    int report[2] = {-1, -1};
    if (::pipe2(report, O_CLOEXEC) != 0) {
        *error = std::string("cannot create a pipe: ") + std::strerror(errno);
        return {};
    }
    const bool started = startDetached(daemon, environment, report[1], error);
    ::close(report[1]);
    Report reported;
    if (started)
        reported = readReport(report[0], deadline);
    ::close(report[0]);
    if (!started)
        return {};
    // A daemon that found another one holding a socket (another command
    // started it at the same time) leaves that one to answer: it took its
    // lock before it binds, so the wait is the same as for this one. Any
    // other failure: one look, no wait.
    const bool waits = reported.state == Report::State::Listening || reported.state == Report::State::Held;
    Reached reached = waits ? waitForDaemon(sockets, deadline, error) : connectAny(sockets, error);
    if (!reached.connection && error->empty()) {
        std::string& reason = reported.message;
        while (reason.ends_with('\n'))
            reason.pop_back();
        switch (reported.state) {
        case Report::State::Failed:
            *error = printable(failureText(reason, daemon));
            break;
        case Report::State::Held:
            *error = printable(reason) + ", which does not answer";
            break;
        case Report::State::Ended:
            *error = "aglow-mux ended before it listened";
            break;
        case Report::State::Listening:
        case Report::State::TimedOut:
            *error = "aglow-mux did not start: no daemon answers on " + paths(sockets);
            break;
        }
    }
    return reached;
}

} // namespace AfterglowMux
