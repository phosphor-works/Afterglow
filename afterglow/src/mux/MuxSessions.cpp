// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mux/MuxSessions.h"

#include "PaneLaunch.h"
#include "app/NotificationCenter.h"
#include "config/Settings.h"
#include "control/RemoteSockets.h"
#include "shell/Aglow.h"

#include "MuxConnect.h"
#include "MuxStart.h"

#include <AfterglowProto/Socket.h>
#include <AfterglowProto/Text.h>

#include <algorithm>
#include <fstream>

#include <unistd.h>

extern char** environ;

namespace Afterglow {

namespace Json = AfterglowProto::Json;
using AfterglowMux::CallTimeoutMs;
using AfterglowMux::connectAny;
using AfterglowProto::IpcConnection;
using AfterglowProto::printable;

/// The aglow-mux next to afterglow, or empty with @p error.
std::string muxDaemonPath(std::string* error)
{
    return Aglow::siblingExecutable("aglow-mux", error);
}

/// A connection to the daemon, which this starts when none answers: the
/// aglow-mux next to afterglow.
AfterglowMux::Reached connectOrStartDaemon(const AfterglowProto::MuxSockets& sockets, std::string* error,
                                           const AfterglowMux::Environment& environment)
{
    if (AfterglowMux::Reached reached = connectAny(sockets, error); reached.connection || !error->empty()) return reached;
    const std::string daemon = muxDaemonPath(error);
    if (daemon.empty()) return {};
    if (::access(daemon.c_str(), X_OK) != 0) {
        *error = "cannot find aglow-mux next to afterglow: " + daemon;
        return {};
    }
    return AfterglowMux::connectOrStart(sockets, daemon, environment, error);
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

/// The environment of the session: this process's. Each pane changes it with
/// its launch, the first one with the values of this command's, and the
/// variables that name another terminal or the shell that ran this command
/// are among a launch's unsets (offerTerminalEnvironment). JSON carries
/// text: a byte of a name, a value, an argument or a directory that is not
/// UTF-8 becomes U+FFFD on the way to the daemon.
std::vector<std::pair<std::string, std::string>> sessionEnvironment()
{
    std::vector<std::pair<std::string, std::string>> env;
    for (char** e = environ; *e; ++e) {
        const std::string_view entry(*e);
        const auto eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        env.emplace_back(std::string(entry.substr(0, eq)), std::string(entry.substr(eq + 1)));
    }
    return env;
}

/// What a session takes from @p settings whatever its host: the program's
/// name for `afterglow mux kill`, the size, the terminal and the Control
/// settings.
void sessionValues(AfterglowMux::SessionOptions& session, const Settings& settings, const QStringList& command)
{
    session.launch.launchedProgram = launchedProgramName(command).toStdString();
    session.launch.command = sessionCommand(command);
    // No window measured the panes: the size of a new window, in cells,
    // which the schema keeps within what a terminal takes.
    session.size = {settings.window.initialColumns, settings.window.initialRows, 0, 0};
    const AfterglowMux::SessionSettings followed = sessionSettings(settings);
    session.terminal = followed.terminal;
    session.notificationCapabilities = NotificationCenter::capabilities();
    session.control = followed.control;
}

AfterglowMux::SessionSettings sessionSettings(const Settings& settings)
{
    return {settings.terminal, {settings.control.enabled, settings.control.allowReadText, remoteCommandsOff(settings).toStdString()}};
}

/// The sessions of the daemon on @p connection, or nullopt with @p error.
std::optional<std::vector<AfterglowMux::SessionInfo>> daemonSessions(IpcConnection& connection, std::string* error)
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

AfterglowMux::SessionOptions localSessionOptions(const Settings& settings, const QString& cwd, const QStringList& command,
                                                 const QString& name)
{
    AfterglowMux::SessionOptions session;
    session.name = name.toStdString();
    AfterglowPty::SpawnOptions launch = prepareLaunch(settings, cwd, command, {});
    offerTerminalEnvironment(launch, settings.term);
    session.environment = sessionEnvironment();
    session.umask = currentUmask();
    session.launch.argv = launch.argv;
    session.launch.workingDirectory = launch.workingDirectory;
    session.launch.environment = launch.environment;
    session.launch.unset = launch.unsetEnvironment;
    sessionValues(session, settings, command);
    return session;
}

LocalDaemon localDaemon()
{
    return {AfterglowProto::muxSockets(), AfterglowMux::currentEnvironment()};
}

QString newLocalSession(const LocalDaemon& daemon, const AfterglowMux::SessionOptions& options, QString* error)
{
    std::string reason;
    AfterglowMux::Reached reached = connectOrStartDaemon(daemon.sockets, &reason, daemon.environment);
    if (!reached.connection) {
        *error = QString::fromStdString(reason.empty() ? std::string("aglow-mux did not start") : printable(reason));
        return {};
    }
    IpcConnection::Error callError;
    const auto result = reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::NewSession,
                                                 Json::Array{AfterglowMux::ProtocolVersion, AfterglowMux::toJson(options)},
                                                 AfterglowMux::NewSessionTimeoutMs, &callError);
    if (!result) {
        *error = QString::fromStdString(printable(callError.message));
        return {};
    }
    const std::string name = (*result)["name"].toString();
    if (!AfterglowMux::validSessionName(name)) {
        *error = QStringLiteral("the daemon sent a session name that is not valid");
        return {};
    }
    return QString::fromStdString(name);
}

bool killLocalSession(const LocalDaemon& daemon, const QString& name)
{
    std::string error;
    AfterglowMux::Reached reached = connectAny(daemon.sockets, &error);
    if (!reached.connection) return false;
    IpcConnection::Error callError;
    return reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::KillSession, Json::Array{name.toStdString()}, CallTimeoutMs,
                                    &callError)
        .has_value();
}

bool localSessionRuns(const LocalDaemon& daemon, const QString& name)
{
    std::string error;
    AfterglowMux::Reached reached = connectAny(daemon.sockets, &error);
    if (!reached.connection) return false;
    const auto list = daemonSessions(*reached.connection, &error);
    return list && std::any_of(list->begin(), list->end(), [&](const auto& info) { return info.name == name.toStdString(); });
}

} // namespace Afterglow
