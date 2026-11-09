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
/// pane itself: @p workingDirectory, or the home directory when it is not a
/// directory; the configured environment, then the launch's; @p command, else
/// Terminal.shell, else the user's shell; shell integration; and
/// AFTERGLOW_SSH_HELPER and AFTERGLOW_SSH_TERMINFO unless the launch names
/// them. A window's panes and a mux session's both start from it. A window
/// adds its own fallback when the program fails to start.
AfterglowPty::SpawnOptions prepareLaunch(const Settings& settings, const QString& workingDirectory, const QStringList& command,
                                         const QVariantMap& environment);

/// TERM, and what goes with it: TERMINFO_DIRS for afterglow's own entry,
/// COLORTERM, TERM_PROGRAM and TERM_PROGRAM_VERSION. Removes the variables
/// that name another terminal or the shell that started this one (TMUX,
/// WINDOWID, KITTY_WINDOW_ID, SHLVL and others). A name that the
/// configuration or the launch sets or unsets keeps its value.
void offerTerminalEnvironment(AfterglowPty::SpawnOptions& options, const QString& term);

/// Applies a launch's own variables to @p options (`env` of a script or a
/// control call): a string sets the name, false unsets it. The launch wins
/// over what @p options had, and each name is set or unset once.
void applyLaunchEnvironment(AfterglowPty::SpawnOptions& options, const QVariantMap& environment);

/// The name that a pane opened for @p command shows as its job while no
/// other job runs (Pane::foregroundJob): the program's file name. Empty for
/// the shell.
QString launchedProgramName(const QStringList& command);

} // namespace Afterglow
