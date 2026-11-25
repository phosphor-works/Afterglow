// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mux/MuxSessions.h"

#include "PaneLaunch.h"
#include "ProjectStore.h"
#include "app/NotificationCenter.h"
#include "config/Settings.h"
#include "config/Theme.h"
#include "control/RemoteSockets.h"
#include "shell/Sibling.h"

#include "MuxConnect.h"
#include "MuxStart.h"

#include <AfterglowProto/Socket.h>
#include <AfterglowProto/Text.h>
#include <AfterglowRender/FontSet.h>

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QScreen>

#include <algorithm>

#include <unistd.h>

extern char** environ;

Q_DECLARE_LOGGING_CATEGORY(lcMux)

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

namespace {

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
        if (eq == std::string_view::npos || eq == 0)
            continue;
        env.emplace_back(std::string(entry.substr(0, eq)), std::string(entry.substr(eq + 1)));
    }
    return env;
}

/// sessionCommand(@p command), with a warning when the session keeps none
/// of a command that it runs: once, where a launch is made, not at each
/// layout that the window sends.
std::vector<std::string> launchCommand(const QStringList& command)
{
    if (!ProjectStore::keepsCommand(command)) {
        qCWarning(lcMux).noquote()
            << QStringLiteral(
                   "the session keeps no command for the pane of \"%1\": more than %2 arguments, one of more than %L3 "
                   "bytes, a NUL, or no program name")
                   .arg(command.first().left(256))
                   .arg(ProjectStore::MaxCommandArgs)
                   .arg(ProjectStore::MaxCommandArgBytes);
    }
    return sessionCommand(command);
}

/// What a session takes from @p settings besides its first launch: the
/// size, the terminal and the Control settings, and Mux.keptNotifications.
void followSettings(AfterglowMux::SessionOptions& session, const Settings& settings)
{
    // No window measured the panes: the size of a new window, which the
    // schema keeps within what a terminal takes.
    session.size = initialPaneSize(settings);
    const AfterglowMux::SessionSettings followed = sessionSettings(settings);
    session.terminal = followed.terminal;
    session.notificationCapabilities = followed.notificationCapabilities;
    session.control = followed.control;
    session.keptNotifications = followed.keptNotifications;
}

} // namespace

/// A connection to the daemon, which this starts when none answers: the
/// aglow-mux next to afterglow.
AfterglowMux::Reached connectOrStartDaemon(const AfterglowProto::MuxSockets& sockets, std::string* error,
                                           const AfterglowMux::Environment& environment,
                                           std::chrono::milliseconds startWait)
{
    // No socket path can be used: no daemon could be reached, nor one that
    // starts.
    if (sockets.candidates.empty()) {
        *error = sockets.error;
        return {};
    }
    if (AfterglowMux::Reached reached = connectAny(sockets, error); reached.connection || !error->empty())
        return reached;
    const std::string daemon = muxDaemonPath(error);
    if (daemon.empty())
        return {};
    if (::access(daemon.c_str(), X_OK) != 0) {
        *error = "cannot find aglow-mux next to afterglow: " + daemon;
        return {};
    }
    return AfterglowMux::connectOrStart(sockets, daemon, environment, error, startWait);
}

std::string callFailure(const IpcConnection::Error& error, std::string_view fn)
{
    if (!error.message.empty())
        return printable(error.message);
    if (error.code.empty())
        return "the mux daemon gave no answer to " + std::string(fn);
    return "the mux daemon refused " + std::string(fn) + " (" + printable(error.code) + ")";
}

AfterglowMux::PaneSize initialPaneSize(const Settings& settings)
{
    AfterglowMux::PaneSize size{settings.window.initialColumns, settings.window.initialRows, 0, 0};
    // Without a cell size, the daemon places no Sixel or iTerm2 image, and a
    // Kitty image moves no cursor: the font's, as a window measures it.
    double ratio = 1.0;
    if (hasGuiApplication()) {
        if (const QScreen* screen = QGuiApplication::primaryScreen())
            ratio = screen->devicePixelRatio();
    }
    try {
        const AfterglowRender::FontSet fonts(settings.font, ratio);
        size.cellWidth = fonts.metrics().cellWidth;
        size.cellHeight = fonts.metrics().cellHeight;
    } catch (const std::exception&) {
        // No font: no cell size, as before.
    }
    return size;
}

/// What a session takes from @p settings whatever its host: the program's
/// name for `afterglow mux kill`, the size, the terminal and the Control
/// settings.
void sessionValues(AfterglowMux::SessionOptions& session, const Settings& settings, const QStringList& command)
{
    session.launch.launchedProgram = launchedProgramName(command).toStdString();
    session.launch.command = launchCommand(command);
    followSettings(session, settings);
}

AfterglowMux::SessionSettings sessionSettings(const Settings& settings, bool environment)
{
    AfterglowMux::SessionSettings out{
        .terminal = settings.terminal,
        .control = {.enabled = settings.control.enabled,
                    .allowReadText = settings.control.allowReadText,
                    .remoteCommandsOff = remoteCommandsOff(settings).toStdString(),
                    .eventInterval = settings.control.eventInterval,
                    .allowAdopt = settings.control.allowAdopt},
        .keptNotifications = settings.mux.keptNotifications,
        .notificationCapabilities = settings.notifications ? NotificationCenter::capabilities() : std::string(),
    };
    if (!environment)
        return out;
    for (const QString& name : settings.mux.updateEnvironment) {
        // The bytes that are looked up are the name that goes: JSON carries
        // UTF-8. A value that is not UTF-8 becomes U+FFFD in JSON, as the
        // session's environment does (sessionEnvironment).
        const QByteArray encoded = name.toUtf8();
        if (qEnvironmentVariableIsSet(encoded.constData()))
            out.environment.emplace_back(encoded.toStdString(), qgetenv(encoded.constData()).toStdString());
        else
            out.environment.emplace_back(encoded.toStdString(), std::nullopt);
    }
    return out;
}

/// The sessions of the daemon on @p connection, or nullopt with @p error.
std::optional<std::vector<AfterglowMux::SessionInfo>> daemonSessions(IpcConnection& connection, std::string* error)
{
    IpcConnection::Error callError;
    const auto result =
        connection.call(AfterglowMux::Target, AfterglowMux::Fn::ListSessions, {}, CallTimeoutMs, &callError);
    if (!result) {
        *error = callFailure(callError, AfterglowMux::Fn::ListSessions);
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

AfterglowMux::Launch localLaunch(const Settings& settings, const QString& cwd, const QStringList& command,
                                 const InheritedEnvironment* inherited, const QVariantMap& environment)
{
    AfterglowPty::SpawnOptions prepared = prepareLaunch(settings, cwd, command, environment, inherited);
    offerTerminalEnvironment(prepared, settings.term, inherited);
    AfterglowMux::Launch launch;
    launch.argv = std::move(prepared.argv);
    launch.workingDirectory = std::move(prepared.workingDirectory);
    launch.environment = std::move(prepared.environment);
    launch.unset = std::move(prepared.unsetEnvironment);
    launch.launchedProgram = launchedProgramName(command).toStdString();
    launch.command = launchCommand(command);
    return launch;
}

AfterglowMux::SessionOptions localSessionOptions(const Settings& settings, const QString& cwd,
                                                 const QStringList& command, const QString& name)
{
    AfterglowMux::SessionOptions session;
    session.name = name.toStdString();
    session.environment = sessionEnvironment();
    // The programs of the session get it. -1: the daemon's own.
    const std::optional<mode_t> mask = currentUmask();
    session.umask = mask ? int(*mask) : -1;
    // Its launch has the program's name and the command: sessionValues
    // would warn a second time of a command that the session cannot keep.
    session.launch = localLaunch(settings, cwd, command);
    followSettings(session, settings);
    return session;
}

std::optional<std::vector<std::uint32_t>> startLocalPanes(const LocalDaemon& daemon, const QString& session,
                                                          const std::vector<AfterglowMux::Launch>& launches,
                                                          const AfterglowMux::PaneSize& size,
                                                          const std::atomic<bool>& stop, QString* error)
{
    std::vector<std::uint32_t> ids;
    if (launches.empty())
        return ids;
    std::string reason;
    AfterglowMux::Reached reached = connectAny(daemon.sockets, &reason);
    if (!reached.connection) {
        if (reason.empty())
            reason = daemon.sockets.candidates.empty() ? daemon.sockets.error : std::string("no mux daemon runs");
        *error = QString::fromStdString(printable(reason));
        return std::nullopt;
    }
    for (const AfterglowMux::Launch& launch : launches) {
        if (stop.load()) {
            *error = QStringLiteral("Afterglow quits");
            return std::nullopt;
        }
        IpcConnection::Error callError;
        const auto result =
            reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::NewPane,
                                     Json::Array{AfterglowMux::ProtocolVersion, session.toStdString(),
                                                 AfterglowMux::toJson(launch), AfterglowMux::toJson(size)},
                                     AfterglowMux::SpawnCallTimeoutMs, &callError);
        const auto id = result ? (*result)["pane"].toInteger() : std::nullopt;
        if (!id || *id <= 0 || *id > 0xffffffffLL) {
            *error = result ? QStringLiteral("the mux daemon gave no pane")
                            : QString::fromStdString(callFailure(callError, AfterglowMux::Fn::NewPane));
            return std::nullopt;
        }
        ids.push_back(std::uint32_t(*id));
    }
    return ids;
}

LocalDaemon localDaemon(const Settings& settings)
{
    return {AfterglowProto::muxSockets(), AfterglowMux::currentEnvironment(),
            std::chrono::seconds(settings.mux.startTimeout)};
}

QString newLocalSession(const LocalDaemon& daemon, const AfterglowMux::SessionOptions& options, QString* error,
                        std::uint32_t* firstPane)
{
    std::string reason;
    AfterglowMux::Reached reached = connectOrStartDaemon(daemon.sockets, &reason, daemon.environment, daemon.startWait);
    if (!reached.connection) {
        *error = QString::fromStdString(reason.empty() ? std::string("aglow-mux did not start") : printable(reason));
        return {};
    }
    IpcConnection::Error callError;
    const auto result =
        reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::NewSession,
                                 Json::Array{AfterglowMux::ProtocolVersion, AfterglowMux::toJson(options)},
                                 AfterglowMux::SpawnCallTimeoutMs, &callError);
    if (!result) {
        *error = QString::fromStdString(callFailure(callError, AfterglowMux::Fn::NewSession));
        return {};
    }
    const std::string name = (*result)["name"].toString();
    if (!AfterglowMux::validSessionName(name)) {
        *error = QStringLiteral("the daemon sent a session name that is not valid");
        return {};
    }
    const auto pane = (*result)["pane"].toInteger();
    if (firstPane)
        *firstPane = pane && *pane > 0 && *pane <= 0xffffffffLL ? std::uint32_t(*pane) : 0;
    return QString::fromStdString(name);
}

bool killLocalSession(const LocalDaemon& daemon, const QString& name)
{
    std::string error;
    AfterglowMux::Reached reached = connectAny(daemon.sockets, &error);
    // No daemon listens: no session runs. A socket that cannot be used, or
    // a daemon that cannot be used, may hold it.
    if (!reached.connection)
        return error.empty() && !daemon.sockets.candidates.empty();
    IpcConnection::Error callError;
    if (reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::KillSession, Json::Array{name.toStdString()},
                                 CallTimeoutMs, &callError))
        return true;
    // It ended before, as a project of one pane whose program exits at once does.
    return callError.code == AfterglowMux::Error::NoSuchSession;
}

bool localSessionRuns(const LocalDaemon& daemon, const QString& name)
{
    std::string error;
    AfterglowMux::Reached reached = connectAny(daemon.sockets, &error);
    if (!reached.connection)
        return false;
    const auto list = daemonSessions(*reached.connection, &error);
    return list && std::any_of(list->begin(), list->end(), [&](const auto& info) {
               return info.name == name.toStdString();
           });
}

} // namespace Afterglow
