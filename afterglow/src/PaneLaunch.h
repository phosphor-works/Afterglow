// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowPty/Pty.h>

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace Afterglow {

struct Settings;

/// The program of a new pane and its environment, before the variables of the
/// pane itself: the configured environment and the launch's, the shell or
/// @p command, shell integration and the ssh helper. A window and a mux
/// session start their panes the same way.
AfterglowPty::SpawnOptions prepareLaunch(const Settings& settings, const QString& workingDirectory, const QStringList& command,
                                         const QVariantMap& environment);

/// TERM, and what goes with it: TERMINFO_DIRS for afterglow's own entry,
/// COLORTERM and TERM_PROGRAM. A name that the configuration or the launch
/// sets or unsets keeps its value.
void offerTerminalEnvironment(AfterglowPty::SpawnOptions& options, const QString& term);

} // namespace Afterglow
