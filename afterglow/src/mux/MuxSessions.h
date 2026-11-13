// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxConnect.h"
#include "MuxProtocol.h"
#include "MuxStart.h"

#include <AfterglowProto/Ipc.h>

#include <QString>
#include <QStringList>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Afterglow {

struct Settings;

// The sessions of the mux daemon of this computer, as `afterglow mux` and a
// project that opens into a session start and find them.

/// The aglow-mux next to afterglow, or empty with @p error.
std::string muxDaemonPath(std::string* error);
/// A connection to the daemon, which this starts when none answers: the
/// aglow-mux next to afterglow, in @p environment.
AfterglowMux::Reached connectOrStartDaemon(const AfterglowProto::MuxSockets& sockets, std::string* error,
                                           const AfterglowMux::Environment& environment = AfterglowMux::currentEnvironment());
/// The file mode creation mask of this process, which the programs of a
/// session get, or -1.
int currentUmask();
/// The environment of a session: this process's.
std::vector<std::pair<std::string, std::string>> sessionEnvironment();
/// What a session takes from @p settings whatever its host.
void sessionValues(AfterglowMux::SessionOptions& session, const Settings& settings, const QStringList& command);
/// The sessions of the daemon on @p connection, or nullopt with @p error.
std::optional<std::vector<AfterglowMux::SessionInfo>> daemonSessions(AfterglowProto::IpcConnection& connection, std::string* error);

/// The session that `afterglow mux new` starts on this computer for a pane
/// in @p cwd, an absolute directory, that runs @p command (empty: the
/// shell), with @p settings. @p name empty: the daemon names it.
AfterglowMux::SessionOptions localSessionOptions(const Settings& settings, const QString& cwd, const QStringList& command,
                                                 const QString& name);
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
/// starts when none runs. Its name, or empty with @p error.
QString newLocalSession(const LocalDaemon& daemon, const AfterglowMux::SessionOptions& options, QString* error);
/// Whether the daemon of this computer runs the session @p name. Starts no
/// daemon.
bool localSessionRuns(const LocalDaemon& daemon, const QString& name);
/// Ends the session @p name of the daemon of this computer, and its
/// programs. Starts no daemon. False when the daemon did not end it.
bool killLocalSession(const LocalDaemon& daemon, const QString& name);

} // namespace Afterglow
