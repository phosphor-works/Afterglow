// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/Mux.h"

#include "PaneLaunch.h"
#include "config/Settings.h"
#include "control/RemoteSockets.h"
#include "shell/Aglow.h"
#include "mux/MuxRoute.h"
#include "mux/MuxSessions.h"
#include "shell/Ssh.h"

#include "MuxConnect.h"
#include "MuxStart.h"
#include "MuxProtocol.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Lock.h>
#include <AfterglowProto/Socket.h>
#include <AfterglowProto/Text.h>
#include <AfterglowPty/CloseFrom.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
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

namespace Afterglow {

namespace {

namespace Json = AfterglowProto::Json;
using AfterglowMux::CallTimeoutMs;
using AfterglowMux::Reached;
using AfterglowMux::connectAny;
using AfterglowProto::IpcConnection;

const char* const Usage = R"(Usage: afterglow mux <command> [options]

Keeps terminal sessions running while no window shows them. The daemon
(aglow-mux) starts with the first session and ends with the last.

Commands:
  server                      Run aglow-mux in the foreground.
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
                              The window of the session elsewhere closes,
                              unless --no-takeover refuses the attach.
  detach [NAME]               Close the window of a session, and keep the
                              session. In a session pane, NAME defaults to
                              the session of that pane.
  ls                          List the sessions.
  kill NAME [--force]         End a session and its programs. With
                              Terminal.confirmClose on, it asks first when a
                              program runs. --force does not ask.

Each command but server takes --ssh DESTINATION: the daemon of that host,
through aglow-mux proxy, which ssh runs there over the connection that
afterglow ssh shares with the host. A session there runs your login shell
with the environment of your ssh login there; --cwd is a directory there.
)";

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
    std::optional<std::string> host; ///< --ssh
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
int openWindow(const std::string& command, const std::string& name, bool takeover, const std::optional<std::string>& host = {})
{
    std::vector<std::string> args{"afterglow", "--mux-attach", name};
    if (!takeover) args.emplace_back("--mux-no-takeover");
    if (host) {
        args.emplace_back("--mux-host");
        args.push_back(*host);
    }
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    ::execv("/proc/self/exe", argv.data());
    const int failure = errno;
    fail(command, std::string("cannot run afterglow: ") + std::strerror(failure));
    return failure == ENOENT ? 127 : 126;
}

/// The way to the daemon of @p host over the shared connection of `afterglow
/// ssh`, or to that of this computer without one. Nothing, with @p error,
/// when ssh cannot read the destination, or no socket path can be used.
std::optional<AfterglowMux::Route> routeTo(const std::optional<std::string>& host, std::string* error)
{
    if (host) {
        QString sshError;
        auto route = muxRoute(QString::fromLocal8Bit(QByteArray::fromStdString(*host)), &sshError);
        if (!route) *error = sshError.toStdString();
        return route;
    }
    AfterglowMux::Route route;
    route.sockets = AfterglowProto::muxSockets();
    if (route.sockets.candidates.empty()) {
        *error = route.sockets.error;
        return std::nullopt;
    }
    return route;
}

/// The daemon of @p host, or of this computer without one. Nothing, with an
/// empty @p error, when no daemon runs: none starts.
Reached reachDaemon(const std::optional<std::string>& host, std::string* error)
{
    const auto route = routeTo(host, error);
    if (!route) return {};
    return AfterglowMux::reach(*route, false, error);
}

/// Takes `--ssh DESTINATION` at @p i of @p argv into @p host. False when the
/// word is something else.
bool takeSsh(int argc, char** argv, int& i, std::optional<std::string>& host, std::string* problem)
{
    if (std::strcmp(argv[i], "--ssh") != 0) return false;
    if (i + 1 == argc) *problem = "--ssh needs a destination";
    else if (host) *problem = "--ssh is given more than once";
    else host = argv[++i];
    return true;
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
        } else if (std::string problem; takeSsh(argc, argv, i, o.host, &problem)) {
            if (!problem.empty()) return usage(problem);
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
    // A directory of another host is that host's to find.
    if (o.host && !o.cwd.isEmpty() && !o.cwd.startsWith(QLatin1Char('/'))) {
        return fail(command, "give an absolute directory of " + printable(*o.host) + " with --cwd");
    }
    std::string error;
    const auto route = routeTo(o.host, &error);
    if (!route) return fail(command, error);

    int qtArgc = 1;
    char* qtArgv[] = {argv[0], nullptr};
    QCoreApplication app(qtArgc, qtArgv);
    // No QGuiApplication: SettingsStore::darkScheme() takes the dark side of
    // a "dark:NAME,light:NAME" theme, whatever the desktop's scheme.
    SettingsStore store;
    const Settings& settings = store.settings();
    AfterglowMux::SessionOptions session;
    session.name = o.name;
    if (o.host) {
        // The environment of the user's ssh login there, which the proxy
        // has, and the user's login shell: none of this computer's paths.
        // The terminal's variables go over it, with afterglow's terminfo
        // entry installed there as afterglow ssh installs it.
        QString term = settings.term;
        if (term == QLatin1String("afterglow") && !Ssh::installTerminfo(QString::fromLocal8Bit(QByteArray::fromStdString(*o.host)))) {
            term = QStringLiteral("xterm-256color");
        }
        AfterglowPty::SpawnOptions terminal;
        offerTerminalEnvironment(terminal, term);
        for (auto& [name, value] : terminal.environment) {
            if (name != "TERMINFO_DIRS") session.environment.emplace_back(std::move(name), std::move(value));
        }
        session.callerEnvironment = true;
        for (const QString& word : o.command) session.launch.argv.push_back(word.toStdString());
        session.launch.workingDirectory = o.cwd.toStdString();
    } else {
        const auto cwd = absolute(o.cwd);
        if (!cwd) return fail(command, "the current directory was removed, so give an absolute directory with --cwd");
        if (!QFileInfo(*cwd).isDir()) return fail(command, "no directory " + printable(cwd->toStdString()));
        session = localSessionOptions(settings, *cwd, o.command, QString::fromStdString(o.name));
    }
    if (o.host) sessionValues(session, settings, o.command);

    Reached reached = o.host ? AfterglowMux::reach(*route, true, &error) : connectOrStartDaemon(route->sockets, &error);
    if (!reached.connection) return fail(command, error.empty() ? std::string("aglow-mux did not start") : error);
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
    // The window's afterglow takes this process: the connection and its
    // proxy end first, and no child is left to it.
    reached = {};
    const int status = openWindow(command, name, true, o.host);
    // afterglow did not run, so no window will show the session: it ends
    // here. (A window that starts and then fails leaves it running.)
    if (Reached again = o.host ? AfterglowMux::reach(*route, false, &error) : connectAny(route->sockets, &error); again.connection) {
        IpcConnection::Error killError;
        (void)again.connection->call(AfterglowMux::Target, AfterglowMux::Fn::KillSession, Json::Array{name}, CallTimeoutMs, &killError);
    }
    return status;
}

int listSessions(int argc, char** argv)
{
    std::optional<std::string> host;
    std::string problem;
    for (int i = 1; i < argc; ++i) {
        if (takeSsh(argc, argv, i, host, &problem)) {
            if (!problem.empty()) return usage(problem);
            continue;
        }
        return usage("ls takes no arguments but --ssh DESTINATION");
    }
    std::string error;
    Reached reached = reachDaemon(host, &error);
    if (!reached.connection && !error.empty()) return fail("ls", error);
    std::vector<AfterglowMux::SessionInfo> list;
    if (reached.connection) {
        auto found = daemonSessions(*reached.connection, &error);
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
    std::optional<std::string> host;
    std::string problem;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--no-takeover") takeover = false;
        else if (takeSsh(argc, argv, i, host, &problem)) {
            if (!problem.empty()) return usage(problem);
        } else if (name.empty() && !arg.starts_with('-')) name = arg;
        else return usage("unknown argument of attach: " + printable(arg));
    }
    if (!name.empty() && !AfterglowMux::validSessionName(name)) {
        return fail("attach", std::string("the name must be ") + AfterglowMux::SessionNameRule, 2);
    }
    if (const std::string problem = displayProblem(); !problem.empty()) return fail("attach", problem);
    if (name.empty()) {
        std::string error;
        Reached reached = reachDaemon(host, &error);
        if (!reached.connection && !error.empty()) return fail("attach", error);
        std::optional<std::vector<AfterglowMux::SessionInfo>> list;
        if (reached.connection) {
            list = daemonSessions(*reached.connection, &error);
            if (!list) return fail("attach", error);
        }
        // No session: a new one, in a window.
        if (!list || list->empty()) {
            reached = {};
            std::vector<std::string> words{"new"};
            if (host) words.insert(words.end(), {"--ssh", *host});
            std::vector<char*> newArgv;
            for (std::string& word : words) newArgv.push_back(word.data());
            newArgv.push_back(nullptr);
            return newSession(int(words.size()), newArgv.data(), "attach");
        }
        if (list->size() > 1) {
            std::string names;
            for (const auto& s : *list) names += (names.empty() ? "" : ", ") + printable(s.name);
            return fail("attach", "more than one session runs: name one of " + names);
        }
        name = list->front().name;
    }
    return openWindow("attach", name, takeover, host);
}

int detachSession(int argc, char** argv)
{
    std::string name;
    std::optional<std::string> host;
    std::string problem;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (takeSsh(argc, argv, i, host, &problem)) {
            if (!problem.empty()) return usage(problem);
            continue;
        }
        if (name.empty() && !arg.starts_with('-')) name = arg;
        else return usage("unknown argument of detach: " + printable(arg));
    }
    // A pane of this computer names no session of another host.
    if (host && name.empty()) return usage("detach --ssh takes the name of a session");
    // In a session pane, the session that holds the pane now: one that
    // broke out of its window keeps the AFTERGLOW_SESSION of its first
    // session. Its AFTERGLOW_PANE_ID is the daemon's.
    const char* own = std::getenv("AFTERGLOW_SESSION");
    const bool inSessionPane = own && *own;
    if (name.empty() && !inSessionPane) return usage("detach takes the name of a session, outside a session pane");
    if (!name.empty() && !AfterglowMux::validSessionName(name)) {
        return fail("detach", std::string("the name must be ") + AfterglowMux::SessionNameRule, 2);
    }
    std::string error;
    Reached reached = reachDaemon(host, &error);
    if (!reached.connection) return fail("detach", error.empty() ? std::string("no sessions") : error);
    IpcConnection::Error callError;
    if (name.empty()) {
        const char* paneId = std::getenv("AFTERGLOW_PANE_ID");
        char* end = nullptr;
        const long long pane = paneId && *paneId ? std::strtoll(paneId, &end, 10) : -1;
        if (pane <= 0 || pane > 0xffffffffLL || *end != '\0') return fail("detach", "AFTERGLOW_PANE_ID names no pane of the mux daemon");
        const auto session = reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::PaneSession, Json::Array{std::int64_t(pane)},
                                                      CallTimeoutMs, &callError);
        if (!session) return fail("detach", printable(callError.message));
        name = session->toString();
        if (!AfterglowMux::validSessionName(name)) return fail("detach", "the daemon sent a session name that is not valid");
    }
    if (!reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::Detach, Json::Array{name}, CallTimeoutMs, &callError)) {
        return fail("detach", printable(callError.message));
    }
    return 0;
}

int killSession(int argc, char** argv)
{
    std::string name;
    bool force = false;
    std::optional<std::string> host;
    std::string problem;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--force") force = true;
        else if (takeSsh(argc, argv, i, host, &problem)) {
            if (!problem.empty()) return usage(problem);
        } else if (name.empty() && !arg.starts_with('-')) name = arg;
        else return usage("unknown argument of kill: " + printable(arg));
    }
    if (name.empty()) return usage("kill takes the name of a session");
    std::string error;
    // One connection for the list, the question and the kill: it keeps the
    // daemon running while the user answers.
    Reached reached = reachDaemon(host, &error);
    if (!reached.connection) return fail("kill", error.empty() ? std::string("no sessions") : error);
    const auto list = daemonSessions(*reached.connection, &error);
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
    const std::string daemon = muxDaemonPath(&error);
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
