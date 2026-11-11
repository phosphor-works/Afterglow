// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/Mux.h"

#include "PaneLaunch.h"
#include "app/NotificationCenter.h"
#include "config/Settings.h"
#include "shell/Aglow.h"

#include "MuxConnect.h"
#include "MuxProtocol.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Lock.h>
#include <AfterglowProto/Socket.h>
#include <AfterglowProto/Text.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
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

namespace Afterglow {

namespace {

namespace Json = AfterglowProto::Json;
using AfterglowMux::CallTimeoutMs;
using AfterglowMux::Reached;
using AfterglowMux::connectAny;
using AfterglowProto::IpcConnection;

const char* const Usage = R"(Usage: afterglow mux <command> [options]

Keeps terminal sessions running while no window shows them. The daemon
(afterglow-mux) starts with the first session and ends with the last.

Commands:
  server                      Run afterglow-mux in the foreground.
  new [options] [-- COMMAND]  Create a session and show it in a window. Options:
        --name NAME           The session name: 1 to 64 ASCII letters,
                              digits, '_' or '-', with a letter, a digit or
                              '_' first. By default the daemon gives a
                              number that it did not give before.
        --cwd DIR             Start in DIR, not in the current directory.
        --detach              Create the session without a window, and
                              print its name.
  attach [NAME] [--no-takeover]
                              Show a session in a window. Without NAME, the
                              only session, or a new one when none runs.
                              The pane of the session in another window
                              closes, unless --no-takeover refuses the
                              attach.
  detach [NAME]               Close the pane of a session in its window, and
                              keep the session. In a session pane, NAME
                              defaults to that session.
  ls                          List the sessions.
  kill NAME [--force]         End a session and its programs. With
                              Terminal.confirmClose on, it asks first when a
                              program runs. --force does not ask.
)";

// The daemon of a client that just started it listens within this time.
constexpr auto StartWait = std::chrono::seconds(5);
// The unit of a daemon that the systemd user manager runs.
constexpr const char* Unit = "afterglow-mux";

int fail(const std::string& command, const std::string& message, int status = 1)
{
    std::fprintf(stderr, "afterglow mux %s: %s\n", command.c_str(), message.c_str());
    return status;
}

int usage(const std::string& message)
{
    if (!message.empty()) std::fprintf(stderr, "afterglow mux: %s\n\n", message.c_str());
    std::fputs(Usage, stderr);
    return 2;
}

using AfterglowProto::printable;

// Whether a daemon holds a socket: it takes the lock before it binds and
// keeps it until it ends.
bool lockHeldElsewhere(const AfterglowProto::MuxSockets& sockets)
{
    return std::any_of(sockets.candidates.cbegin(), sockets.candidates.cend(),
                       [](const AfterglowProto::MuxSocket& socket) { return AfterglowProto::lockFileHeld(socket.path + ".lock"); });
}

std::string daemonPath(std::string* error)
{
    return Aglow::siblingExecutable("afterglow-mux", error);
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
    // runMux() set SIGCHLD to its default, so the status is the child's. A
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
        ::close_range(4, ~0U, 0);
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

/// A connection to the daemon, which this starts when none answers.
Reached connectOrStart(const AfterglowProto::MuxSockets& sockets, std::string* error)
{
    if (Reached reached = connectAny(sockets, error); reached.connection || !error->empty()) return reached;
    const std::string daemon = daemonPath(error);
    if (daemon.empty()) return {};
    if (::access(daemon.c_str(), X_OK) != 0) {
        *error = "cannot find afterglow-mux next to afterglow: " + daemon;
        return {};
    }
    const auto deadline = std::chrono::steady_clock::now() + StartWait;
    if (userManager()) {
        std::vector<std::string> start = {"systemd-run", "--user", "--quiet", "--collect",
                                          std::string("--unit=") + Unit, "--description=Afterglow terminal sessions"};
        // The daemon runs in the user manager's environment, and finds its
        // state directory's socket from these: the one that this command and
        // later clients look for. TMPDIR is the sessions' own: the daemon
        // deletes the temporary files of kitty images (t=t) that its
        // programs write there, as a window does.
        for (const char* name : {"XDG_STATE_HOME", "HOME", "TMPDIR"}) {
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

/// The file mode creation mask of this process, which the programs of the
/// session get, or -1. Read from /proc: umask() would change it to read it.
int currentUmask()
{
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);) {
        if (!line.starts_with("Umask:")) continue;
        try {
            return std::stoi(line.substr(6), nullptr, 8);
        } catch (const std::exception&) {
            return -1;
        }
    }
    return -1;
}

/// The whole environment that the session's programs get: this command's,
/// with the values of the launch. The variables that name another terminal
/// or the shell that ran this command are among the launch's unsets
/// (offerTerminalEnvironment). JSON carries text: a byte of a name, a value,
/// an argument or a directory that is not UTF-8 becomes U+FFFD on the way to
/// the daemon.
std::vector<std::pair<std::string, std::string>> sessionEnvironment(const AfterglowPty::SpawnOptions& opts)
{
    std::vector<std::pair<std::string, std::string>> env;
    for (char** e = environ; *e; ++e) {
        const std::string_view entry(*e);
        const auto eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        env.emplace_back(std::string(entry.substr(0, eq)), std::string(entry.substr(eq + 1)));
    }
    const auto drop = [&env](const std::string& name) {
        std::erase_if(env, [&](const auto& kv) { return kv.first == name; });
    };
    for (const auto& name : opts.unsetEnvironment) drop(name);
    for (const auto& [name, value] : opts.environment) {
        drop(name);
        env.emplace_back(name, value);
    }
    return env;
}

/// @p path as an absolute path, or nullopt when it is relative and the
/// current directory was removed (it has no path then). Not QFileInfo: it
/// takes ":dir" for a Qt resource. Unlike the absolute() of `afterglow ctl`,
/// which leaves "~" to the running afterglow, this resolves every path: the
/// daemon starts the program in exactly this directory.
std::optional<QString> absolute(const QString& path)
{
    if (path.startsWith(QLatin1Char('/'))) return QDir::cleanPath(path);
    const QString here = QDir::currentPath();
    if (here.isEmpty() || !here.startsWith(QLatin1Char('/'))) return std::nullopt;
    return QDir::cleanPath(path.isEmpty() ? here : here + QLatin1Char('/') + path);
}

struct NewOptions
{
    std::string name;
    QString cwd;
    bool detach = false;
    QStringList command;
};

/// Why a window could not open here, or empty. Afterglow shows windows only
/// on Wayland: without a display, a window of a session would fail after the
/// session was made. The display is libwayland's: WAYLAND_DISPLAY, else
/// wayland-0, in XDG_RUNTIME_DIR unless it is a path.
std::string displayProblem()
{
    if (const char* platform = std::getenv("QT_QPA_PLATFORM"); platform && *platform && !std::string_view(platform).starts_with("wayland")) {
        return std::string("QT_QPA_PLATFORM is ") + printable(platform) + ", and Afterglow shows windows only on Wayland";
    }
    const char* named = std::getenv("WAYLAND_DISPLAY");
    std::string display = named && *named ? named : "wayland-0";
    if (!display.starts_with('/')) {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || *runtime != '/') return "no Wayland display shows a window here: XDG_RUNTIME_DIR is not set";
        display = std::string(runtime) + "/" + display;
    }
    struct stat info{};
    if (::stat(display.c_str(), &info) != 0 || !S_ISSOCK(info.st_mode)) {
        return "no Wayland display shows a window here: " + printable(display) + " is no socket";
    }
    return {};
}

/// Shows @p name in a window: afterglow runs with --mux-attach, and hands the
/// window to the main process of the display as any launch does. Returns
/// only on a failure.
int openWindow(const std::string& command, const std::string& name, bool takeover)
{
    std::vector<std::string> args{"afterglow", "--mux-attach", name};
    if (!takeover) args.emplace_back("--mux-no-takeover");
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    ::execv("/proc/self/exe", argv.data());
    const int failure = errno;
    fail(command, std::string("cannot run afterglow: ") + std::strerror(failure));
    return failure == ENOENT ? 127 : 126;
}

/// `afterglow mux new`, also for `attach` when no session runs: @p command
/// names the command in messages.
int newSession(int argc, char** argv, const std::string& command = "new")
{
    NewOptions o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") {
            for (++i; i < argc; ++i) o.command << QString::fromLocal8Bit(argv[i]);
            break;
        }
        if (arg == "--name" || arg == "--cwd") {
            if (i + 1 == argc) return usage(arg + " needs a value");
            if (arg == "--name") o.name = argv[++i];
            else o.cwd = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--detach") {
            o.detach = true;
        } else if (arg == "--ssh") {
            return fail(command, "sessions on other hosts are not available yet");
        } else {
            return usage("unknown option of new: " + printable(arg));
        }
    }
    if (!o.name.empty() && !AfterglowMux::validSessionName(o.name)) {
        return fail(command, std::string("the name must be ") + AfterglowMux::SessionNameRule, 2);
    }
    if (!o.detach) {
        if (const std::string problem = displayProblem(); !problem.empty()) return fail(command, problem + ": give --detach");
    }
    const auto sockets = AfterglowProto::muxSockets();
    if (sockets.candidates.empty()) return fail(command, sockets.error);

    int qtArgc = 1;
    char* qtArgv[] = {argv[0], nullptr};
    QCoreApplication app(qtArgc, qtArgv);
    // No QGuiApplication: SettingsStore::darkScheme() takes the dark side of
    // a "dark:NAME,light:NAME" theme, whatever the desktop's scheme.
    SettingsStore store;
    const Settings& settings = store.settings();
    const auto cwd = absolute(o.cwd);
    if (!cwd) return fail(command, "the current directory was removed, so give an absolute directory with --cwd");
    if (!QFileInfo(*cwd).isDir()) return fail(command, "no directory " + printable(cwd->toStdString()));

    AfterglowPty::SpawnOptions launch = prepareLaunch(settings, *cwd, o.command, {});
    offerTerminalEnvironment(launch, settings.term);
    AfterglowMux::SessionOptions session;
    session.name = o.name;
    session.launch.argv = launch.argv;
    session.launch.workingDirectory = launch.workingDirectory;
    session.launch.environment = sessionEnvironment(launch);
    session.launch.launchedProgram = launchedProgramName(o.command).toStdString();
    session.launch.umask = currentUmask();
    // No window measured the panes: the size of a new window, in cells,
    // which the schema keeps within what a terminal takes.
    session.size = {settings.window.initialColumns, settings.window.initialRows, 0, 0};
    session.terminal = settings.terminal;
    session.notificationCapabilities = NotificationCenter::capabilities();

    std::string error;
    Reached reached = connectOrStart(sockets, &error);
    if (!reached.connection) return fail(command, error);
    IpcConnection::Error callError;
    const auto result = reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::NewSession,
                                                 Json::Array{AfterglowMux::ProtocolVersion, AfterglowMux::toJson(session)},
                                                 AfterglowMux::NewSessionTimeoutMs, &callError);
    if (!result) return fail(command, printable(callError.message));
    const std::string name = (*result)["name"].toString();
    if (!AfterglowMux::validSessionName(name)) return fail(command, "the daemon sent a session name that is not valid");
    if (o.detach) {
        std::printf("%s\n", name.c_str());
        return 0;
    }
    const int status = openWindow(command, name, true);
    // afterglow did not run, so no window will show the session: it ends
    // here. (A window that starts and then fails leaves it running.)
    IpcConnection::Error killError;
    (void)reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::KillSession, Json::Array{name}, CallTimeoutMs, &killError);
    return status;
}

/// The sessions of the daemon on @p connection, or nullopt with @p error.
std::optional<std::vector<AfterglowMux::SessionInfo>> sessions(IpcConnection& connection, std::string* error)
{
    IpcConnection::Error callError;
    const auto result = connection.call(AfterglowMux::Target, AfterglowMux::Fn::ListSessions, {}, CallTimeoutMs, &callError);
    if (!result) {
        *error = printable(callError.message);
        return std::nullopt;
    }
    if (!result->isArray()) {
        *error = "the daemon's list of sessions is not a list";
        return std::nullopt;
    }
    std::vector<AfterglowMux::SessionInfo> list;
    for (const Json::Value& item : result->toArray()) {
        auto info = AfterglowMux::sessionInfo(item);
        if (!info || !AfterglowMux::validSessionName(info->name)) {
            *error = "the daemon sent a session that this afterglow cannot read";
            return std::nullopt;
        }
        list.push_back(std::move(*info));
    }
    return list;
}

int listSessions(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ssh") == 0) return fail("ls", "sessions on other hosts are not available yet");
    }
    if (argc != 1) return usage("ls takes no arguments");
    const auto sockets = AfterglowProto::muxSockets();
    if (sockets.candidates.empty()) return fail("ls", sockets.error);
    std::string error;
    Reached reached = connectAny(sockets, &error);
    if (!reached.connection && !error.empty()) return fail("ls", error);
    std::vector<AfterglowMux::SessionInfo> list;
    if (reached.connection) {
        auto found = sessions(*reached.connection, &error);
        if (!found) return fail("ls", error);
        list = std::move(*found);
    }
    if (list.empty()) {
        std::fprintf(stderr, "afterglow mux ls: no sessions\n");
        return 0;
    }
    std::size_t width = 4;
    for (const auto& s : list) width = std::max(width, s.name.size());
    std::printf("%-*s  %-8s  %5s\n", int(width), "NAME", "STATE", "PANES");
    for (const auto& s : list) {
        std::printf("%-*s  %-8s  %5d\n", int(width), printable(s.name).c_str(), s.attached ? "attached" : "detached",
                    s.panes);
    }
    return 0;
}

int attachSession(int argc, char** argv)
{
    std::string name;
    bool takeover = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--no-takeover") takeover = false;
        else if (arg == "--ssh") return fail("attach", "sessions on other hosts are not available yet");
        else if (name.empty() && !arg.starts_with('-')) name = arg;
        else return usage("unknown argument of attach: " + printable(arg));
    }
    if (!name.empty() && !AfterglowMux::validSessionName(name)) {
        return fail("attach", std::string("the name must be ") + AfterglowMux::SessionNameRule, 2);
    }
    if (const std::string problem = displayProblem(); !problem.empty()) return fail("attach", problem);
    if (name.empty()) {
        const auto sockets = AfterglowProto::muxSockets();
        if (sockets.candidates.empty()) return fail("attach", sockets.error);
        std::string error;
        Reached reached = connectAny(sockets, &error);
        if (!reached.connection && !error.empty()) return fail("attach", error);
        std::optional<std::vector<AfterglowMux::SessionInfo>> list;
        if (reached.connection) {
            list = sessions(*reached.connection, &error);
            if (!list) return fail("attach", error);
        }
        // No session: a new one, in a window.
        if (!list || list->empty()) {
            char newArg[] = "new";
            char* newArgv[] = {newArg, nullptr};
            return newSession(1, newArgv, "attach");
        }
        if (list->size() > 1) {
            std::string names;
            for (const auto& s : *list) names += (names.empty() ? "" : ", ") + printable(s.name);
            return fail("attach", "more than one session runs: name one of " + names);
        }
        name = list->front().name;
    }
    return openWindow("attach", name, takeover);
}

int detachSession(int argc, char** argv)
{
    std::string name;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--ssh") return fail("detach", "sessions on other hosts are not available yet");
        if (name.empty() && !arg.starts_with('-')) name = arg;
        else return usage("unknown argument of detach: " + printable(arg));
    }
    // In a session pane: that session.
    if (name.empty()) {
        if (const char* own = std::getenv("AFTERGLOW_SESSION"); own && *own) name = own;
    }
    if (name.empty()) return usage("detach takes the name of a session, outside a session pane");
    if (!AfterglowMux::validSessionName(name)) return fail("detach", std::string("the name must be ") + AfterglowMux::SessionNameRule, 2);
    const auto sockets = AfterglowProto::muxSockets();
    if (sockets.candidates.empty()) return fail("detach", sockets.error);
    std::string error;
    Reached reached = connectAny(sockets, &error);
    if (!reached.connection) return fail("detach", error.empty() ? std::string("no sessions") : error);
    IpcConnection::Error callError;
    if (!reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::Detach, Json::Array{name}, CallTimeoutMs, &callError)) {
        return fail("detach", printable(callError.message));
    }
    return 0;
}

int killSession(int argc, char** argv)
{
    std::string name;
    bool force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--force") force = true;
        else if (arg == "--ssh") return fail("kill", "sessions on other hosts are not available yet");
        else if (name.empty() && !arg.starts_with('-')) name = arg;
        else return usage("unknown argument of kill: " + printable(arg));
    }
    if (name.empty()) return usage("kill takes the name of a session");
    const auto sockets = AfterglowProto::muxSockets();
    if (sockets.candidates.empty()) return fail("kill", sockets.error);
    std::string error;
    // One connection for the list, the question and the kill: it keeps the
    // daemon running while the user answers.
    Reached reached = connectAny(sockets, &error);
    if (!reached.connection) return fail("kill", error.empty() ? std::string("no sessions") : error);
    const auto list = sessions(*reached.connection, &error);
    if (!list) return fail("kill", error);
    const auto found = std::find_if(list->begin(), list->end(), [&](const auto& s) { return s.name == name; });
    if (found == list->end()) return fail("kill", "no session named '" + printable(name) + "'");
    if (!force && !found->jobs.empty()) {
        int qtArgc = 1;
        char* qtArgv[] = {argv[0], nullptr};
        QCoreApplication app(qtArgc, qtArgv);
        SettingsStore store;
        if (store.settings().confirmClose) {
            std::string jobs;
            for (const auto& job : found->jobs) jobs += (jobs.empty() ? "" : ", ") + printable(job);
            if (!::isatty(STDIN_FILENO)) {
                return fail("kill", "session '" + printable(name) + "' runs " + jobs + ": give --force to end it");
            }
            std::fprintf(stderr, "End session '%s', which runs %s? [y/N] ", printable(name).c_str(), jobs.c_str());
            std::string answer;
            if (!std::getline(std::cin, answer) || (answer != "y" && answer != "Y" && answer != "yes")) return 1;
        }
    }
    IpcConnection::Error callError;
    if (!reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::KillSession, Json::Array{name}, CallTimeoutMs, &callError)) {
        return fail("kill", printable(callError.message));
    }
    return 0;
}

int server(int argc, char**)
{
    if (argc != 1) return usage("server takes no arguments");
    std::string error;
    const std::string daemon = daemonPath(&error);
    if (daemon.empty()) return fail("server", error, 127);
    char* const args[] = {const_cast<char*>(daemon.c_str()), nullptr};
    ::execv(daemon.c_str(), args);
    const int failure = errno;
    fail("server", "cannot run " + daemon + ": " + std::strerror(failure));
    return failure == ENOENT || failure == ENOTDIR ? 127 : 126;
}

} // namespace

int runMux(int argc, char** argv)
{
    // A SIGCHLD that this process got as ignored (from its parent, across
    // exec) makes the kernel reap the children of run() and startDetached():
    // their status would be lost, and a systemd-run that worked would look
    // failed.
    std::signal(SIGCHLD, SIG_DFL);
    if (argc < 2) return usage({});
    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        std::fputs(Usage, stdout);
        return 0;
    }
    if (command == "server") return server(argc - 1, argv + 1);
    if (command == "new") return newSession(argc - 1, argv + 1);
    if (command == "ls") return listSessions(argc - 1, argv + 1);
    if (command == "kill") return killSession(argc - 1, argv + 1);
    if (command == "attach") return attachSession(argc - 1, argv + 1);
    if (command == "detach") return detachSession(argc - 1, argv + 1);
    return usage("unknown command: " + printable(command));
}

} // namespace Afterglow
