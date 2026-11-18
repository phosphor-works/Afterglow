// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ProcessUmask.h"
#include "shell/ShellIntegration.h"

#include "MuxConnect.h"
#include "MuxProtocol.h"
#include "MuxStart.h"

#include <AfterglowProto/Ipc.h>

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/types.h>

namespace Afterglow {

struct Settings;

// The sessions of the mux daemon, as `afterglow mux` and a project that opens
// into a session start and find them. The functions named Local, with
// localLaunch, localSessionOptions and localDaemon, are for the daemon of
// this computer. The others serve a session of another host too.

/// The aglow-mux next to afterglow, or empty with @p error.
std::string muxDaemonPath(std::string* error);
/// A connection to the daemon, which this starts when none answers: the
/// aglow-mux next to afterglow, in @p environment. Nothing, with
/// @p sockets' error, when no socket path can be used.
AfterglowMux::Reached
connectOrStartDaemon(const AfterglowProto::MuxSockets& sockets, std::string* error,
                     const AfterglowMux::Environment& environment = AfterglowMux::currentEnvironment());
/// Why the daemon's call @p fn failed, for the user: its message, made
/// printable, or a text that names the call when it gave none. Never empty:
/// callers read an empty error as success.
std::string callFailure(const AfterglowProto::IpcConnection::Error& error, std::string_view fn);
/// The size of a new pane of a session before a window measures it:
/// Window.initialColumns by Window.initialRows cells of the font of
/// @p settings (Font). The cells are in device pixels: at the scale of the
/// primary screen in Afterglow, and of 1 in a command without one. A
/// window then measures its own. 0 by 0 pixels when the font does not load.
AfterglowMux::PaneSize initialPaneSize(const Settings& settings);
/// What a session takes from @p settings whatever its host.
void sessionValues(AfterglowMux::SessionOptions& session, const Settings& settings, const QStringList& command);
/// The settings that a session follows, from @p settings: its terminal
/// values, the Control settings of `afterglow ctl` and `afterglow ssh`,
/// Mux.keptNotifications, and with @p environment the variables of
/// Mux.updateEnvironment as this process has them (for a session of this
/// computer only).
AfterglowMux::SessionSettings sessionSettings(const Settings& settings, bool environment = false);
/// The sessions of the daemon on @p connection, or nullopt with @p error.
std::optional<std::vector<AfterglowMux::SessionInfo>> daemonSessions(AfterglowProto::IpcConnection& connection,
                                                                     std::string* error);

/// What a pane of a session of this computer runs: @p command (empty: the
/// shell) in @p cwd, with the launch's own @p environment, prepared with
/// @p settings, as a window prepares one. It reads the variables that the
/// program inherits in @p inherited: the session's environment, to which
/// the daemon applies what the launch sets and unsets, or this process's
/// without it.
AfterglowMux::Launch localLaunch(const Settings& settings, const QString& cwd, const QStringList& command,
                                 const InheritedEnvironment* inherited = nullptr, const QVariantMap& environment = {});

/// The session that `afterglow mux new`, or a project that opens into a
/// new session, starts on this computer for a pane in @p cwd, an absolute
/// directory, that runs @p command (empty: the shell), with @p settings.
/// Its size is initialPaneSize(). @p name empty: the daemon names it.
AfterglowMux::SessionOptions localSessionOptions(const Settings& settings, const QString& cwd,
                                                 const QStringList& command, const QString& name);
/// What the calls to the daemon of this computer read from the process: its
/// sockets, and the environment that starts it. Taken on the thread that
/// changes the environment, for the calls on another thread.
struct LocalDaemon
{
    AfterglowProto::MuxSockets sockets;
    AfterglowMux::Environment environment;
};
LocalDaemon localDaemon();

/// Starts the session @p options in the daemon of this computer, which it
/// starts when none runs. Its name, with the daemon's ID of its first pane
/// in @p firstPane, or empty with @p error.
QString newLocalSession(const LocalDaemon& daemon, const AfterglowMux::SessionOptions& options, QString* error,
                        std::uint32_t* firstPane = nullptr);
/// Starts a pane for each of @p launches in session @p session of the daemon
/// of this computer, at @p size, each a tab of its own. Their IDs, in the
/// order of @p launches, or nothing with @p error at the first that the
/// daemon refuses, or once @p stop is set (the panes started before stay).
std::optional<std::vector<std::uint32_t>> startLocalPanes(const LocalDaemon& daemon, const QString& session,
                                                          const std::vector<AfterglowMux::Launch>& launches,
                                                          const AfterglowMux::PaneSize& size,
                                                          const std::atomic<bool>& stop, QString* error);
/// Whether the daemon of this computer runs the session @p name. Starts no
/// daemon.
bool localSessionRuns(const LocalDaemon& daemon, const QString& name);
/// Ends the session @p name of the daemon of this computer, and its
/// programs. Starts no daemon. True also when it ran no more (no such
/// session, no daemon). False when it may still run: the daemon refused, or
/// did not answer.
bool killLocalSession(const LocalDaemon& daemon, const QString& name);

} // namespace Afterglow
