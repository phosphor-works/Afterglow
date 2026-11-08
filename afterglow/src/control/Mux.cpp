// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/Mux.h"

#include "PaneLaunch.h"
#include "config/Settings.h"

#include "MuxProtocol.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Socket.h>
#include <AfterglowProto/Text.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace Afterglow {

namespace {

namespace Json = AfterglowProto::Json;
using AfterglowProto::IpcConnection;

const char* const Usage = R"(Usage: afterglow mux <command> [options]

Keeps terminal sessions running while no window shows them. The daemon
(afterglow-mux) starts with the first session and ends with the last.

Commands:
  server                      Run afterglow-mux in the foreground.
  new [options] [-- COMMAND]  Create a session. Options:
        --name NAME           The session name: 1 to 64 letters, digits, '_'
                              or '-'. By default the smallest free number.
        --cwd DIR             Start in DIR, not in the current directory.
        --detach              Create the session without a window.
  ls                          List the sessions.
  kill NAME [--force]         End a session and its programs. With
                              Terminal.confirmClose on, it asks first when a
                              program runs. --force does not ask.
)";

// The daemon of a client that just started it listens within this time.
constexpr auto StartWait = std::chrono::seconds(5);

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

/// Text from a program or a session (a job's name): no control characters
/// reach the terminal of the user.
std::string printable(std::string_view text)
{
    std::string out;
    std::size_t i = 0;
    char32_t cp = 0;
    while (i < text.size()) {
        if (!AfterglowProto::nextUtf8(text, i, cp) || cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0)) {
            out += '?';
            continue;
        }
        AfterglowProto::appendUtf8(out, cp);
    }
    return out;
}

std::string selfDirectory()
{
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::string() : self.parent_path().string();
}

/// The afterglow-mux of the same build or package: never one on PATH.
std::string daemonPath()
{
    const std::string directory = selfDirectory();
    return directory.empty() ? std::string() : directory + "/afterglow-mux";
}

std::unique_ptr<IpcConnection> tryConnect(const std::string& socket, IpcConnection::Error* error)
{
    auto connection = std::make_unique<IpcConnection>();
    if (!connection->connect(socket, error, 1000)) return nullptr;
    return connection;
}

/// Whether the systemd user manager runs the daemon: it has its own unit
/// there, so systemd-oomd stopping the unit of a window leaves the sessions.
/// Only for the user's own runtime directory: a daemon of another one (a
/// test's) must not take the unit name of the user's daemon.
bool userManager()
{
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || std::string(runtime) != "/run/user/" + std::to_string(::getuid())) return false;
    struct stat info{};
    return ::stat((std::string(runtime) + "/systemd/private").c_str(), &info) == 0 && S_ISSOCK(info.st_mode);
}

/// Runs @p argv with the standard streams at /dev/null, and waits for it.
int runQuietly(const std::vector<std::string>& argv)
{
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd : {0, 1}) posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) return -1;
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/// Starts @p daemon in a session of its own, outside the process group and
/// the terminal of this command: it stays when they end.
bool startDetached(const std::string& daemon, std::string* error)
{
    const pid_t child = ::fork();
    if (child < 0) {
        *error = std::string("cannot fork: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        ::setsid();
        // A second fork: the daemon is no session leader, so it can never
        // take a controlling terminal.
        if (::fork() != 0) ::_exit(0);
        const int null = ::open("/dev/null", O_RDWR);
        for (int fd = 0; fd < 3; ++fd) ::dup2(null, fd);
        if (null > 2) ::close(null);
        (void)!::chdir("/");
        char* const args[] = {const_cast<char*>(daemon.c_str()), nullptr};
        ::execv(daemon.c_str(), args);
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    return true;
}

/// A connection to the daemon, which this starts when none listens.
std::unique_ptr<IpcConnection> connectOrStart(const AfterglowProto::MuxSocket& socket, std::string* error)
{
    IpcConnection::Error connectError;
    if (auto connection = tryConnect(socket.path, &connectError)) return connection;
    if (!connectError.noServer) {
        *error = connectError.message;
        return nullptr;
    }
    const std::string daemon = daemonPath();
    if (daemon.empty() || ::access(daemon.c_str(), X_OK) != 0) {
        *error = "cannot find afterglow-mux next to afterglow" + (daemon.empty() ? std::string() : ": " + daemon);
        return nullptr;
    }
    // When systemd-run fails, a daemon of a session of its own. A unit that
    // exists already (another command starts the daemon at the same time)
    // fails too: then the second daemon finds the socket taken and ends.
    if (!userManager()
        || runQuietly({"systemd-run", "--user", "--quiet", "--collect", "--unit=afterglow-mux",
                       "--description=Afterglow terminal sessions", daemon})
            != 0) {
        if (!startDetached(daemon, error)) return nullptr;
    }
    const auto deadline = std::chrono::steady_clock::now() + StartWait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto connection = tryConnect(socket.path, &connectError)) return connection;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    *error = "afterglow-mux did not start: no daemon listens on " + socket.path;
    return nullptr;
}

/// The whole environment that the session's programs get: this command's,
/// with the values of the launch.
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

struct NewOptions
{
    std::string name;
    QString cwd;
    bool detach = false;
    QStringList command;
};

int newSession(int argc, char** argv)
{
    NewOptions o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") {
            for (++i; i < argc; ++i) o.command << QString::fromLocal8Bit(argv[i]);
            break;
        }
        if (arg == "--name" && i + 1 < argc) {
            o.name = argv[++i];
        } else if (arg == "--cwd" && i + 1 < argc) {
            o.cwd = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--detach") {
            o.detach = true;
        } else if (arg == "--ssh") {
            return fail("new", "sessions on other hosts are not available yet");
        } else {
            return usage("unknown option of new: " + printable(arg));
        }
    }
    if (!o.detach) return fail("new", "a window cannot show a session yet: give --detach");
    if (!o.name.empty() && !AfterglowMux::validSessionName(o.name)) {
        return fail("new", "the name must be 1 to 64 letters, digits, underscores or hyphens", 2);
    }
    const auto socket = AfterglowProto::muxSocket();
    if (socket.path.empty()) return fail("new", socket.error);

    int qtArgc = 1;
    char* qtArgv[] = {argv[0], nullptr};
    QCoreApplication app(qtArgc, qtArgv);
    SettingsStore store;
    const Settings& settings = store.settings();
    const QString cwd = o.cwd.isEmpty() ? QDir::currentPath() : QFileInfo(o.cwd).absoluteFilePath();
    if (!QFileInfo(cwd).isDir()) return fail("new", "no directory " + printable(cwd.toStdString()));

    AfterglowPty::SpawnOptions launch = prepareLaunch(settings, cwd, o.command, {});
    offerTerminalEnvironment(launch, settings.term);
    AfterglowMux::SessionOptions session;
    session.name = o.name;
    session.launch.argv = launch.argv;
    session.launch.workingDirectory = launch.workingDirectory;
    session.launch.environment = sessionEnvironment(launch);
    if (!o.command.isEmpty()) session.launch.launchedProgram = QFileInfo(o.command.first()).fileName().toStdString();
    // No window measured the panes: the size of a new window, in cells.
    session.size = {std::clamp(settings.window.initialColumns, 2, 4096), std::clamp(settings.window.initialRows, 1, 4096), 0, 0};
    session.terminal = settings.terminal;

    std::string error;
    auto connection = connectOrStart(socket, &error);
    if (!connection) return fail("new", error);
    IpcConnection::Error callError;
    const auto result = connection->call(AfterglowMux::Target, AfterglowMux::Fn::NewSession,
                                         Json::Array{AfterglowMux::ProtocolVersion, AfterglowMux::toJson(session)}, 10000, &callError);
    if (!result) return fail("new", callError.message);
    std::printf("%s\n", printable((*result)["name"].toString()).c_str());
    return 0;
}

/// The sessions, or nullopt with @p error. No daemon means no session.
std::optional<std::vector<AfterglowMux::SessionInfo>> sessions(const AfterglowProto::MuxSocket& socket, std::string* error)
{
    IpcConnection::Error callError;
    auto connection = tryConnect(socket.path, &callError);
    if (!connection) {
        if (callError.noServer) return std::vector<AfterglowMux::SessionInfo>();
        *error = callError.message;
        return std::nullopt;
    }
    const auto result = connection->call(AfterglowMux::Target, AfterglowMux::Fn::ListSessions, {}, 5000, &callError);
    if (!result) {
        *error = callError.message;
        return std::nullopt;
    }
    std::vector<AfterglowMux::SessionInfo> list;
    for (const Json::Value& item : result->toArray()) {
        if (auto info = AfterglowMux::sessionInfo(item)) list.push_back(std::move(*info));
    }
    return list;
}

int listSessions(int argc, char**)
{
    if (argc != 1) return usage("ls takes no arguments");
    const auto socket = AfterglowProto::muxSocket();
    if (socket.path.empty()) return fail("ls", socket.error);
    std::string error;
    const auto list = sessions(socket, &error);
    if (!list) return fail("ls", error);
    if (list->empty()) {
        std::fprintf(stderr, "afterglow mux ls: no sessions\n");
        return 0;
    }
    std::size_t width = 4;
    for (const auto& s : *list) width = std::max(width, s.name.size());
    std::printf("%-*s  %-8s  %7s  %5s\n", int(width), "NAME", "STATE", "WINDOWS", "PANES");
    for (const auto& s : *list) {
        std::printf("%-*s  %-8s  %7d  %5d\n", int(width), printable(s.name).c_str(), s.attached ? "attached" : "detached",
                    s.windows, s.panes);
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
    const auto socket = AfterglowProto::muxSocket();
    if (socket.path.empty()) return fail("kill", socket.error);
    std::string error;
    const auto list = sessions(socket, &error);
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
    auto connection = tryConnect(socket.path, &callError);
    if (!connection) return fail("kill", callError.message);
    if (!connection->call(AfterglowMux::Target, AfterglowMux::Fn::KillSession, Json::Array{name}, 5000, &callError)) {
        return fail("kill", callError.message);
    }
    return 0;
}

int server(int argc, char**)
{
    if (argc != 1) return usage("server takes no arguments");
    const std::string daemon = daemonPath();
    char* const args[] = {const_cast<char*>(daemon.c_str()), nullptr};
    ::execv(daemon.c_str(), args);
    const int failure = errno;
    fail("server", "cannot run " + daemon + ": " + std::strerror(failure));
    return failure == ENOENT || failure == ENOTDIR ? 127 : 126;
}

} // namespace

int runMux(int argc, char** argv)
{
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
    if (command == "attach" || command == "detach") return fail(command, "a window cannot show a session yet");
    return usage("unknown command: " + printable(command));
}

} // namespace Afterglow
