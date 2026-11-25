// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shell/ShellIntegration.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace Afterglow {

struct Settings;

/// @p path with a leading ~ as the home directory.
QString expandHome(const QString& path);

/// The program of a new tab or split of a session, prepared as a window
/// that shows the session prepares one, with @p settings and over
/// @p inherited, the session's environment: the daemon runs it there. Shell
/// integration reads the shell and the saved variables from it, not from
/// the caller's environment (ctl can run from an ssh login or cron).
/// Without @p inherited, the daemon gave none: this process's. Without
/// @p directory, it starts where the pane is (Window.workingDirectory
/// inherit), which the daemon knows. @p environment is the `env` of the
/// launch options (CONTROL.md): a string sets a variable, false removes it.
/// The last argument of the split and newTab calls of the daemon's control
/// target, for `afterglow ctl --session` and `afterglow mcp`.
QJsonObject sessionLaunch(const Settings& settings, QString directory, const QStringList& command,
                          const InheritedEnvironment* inherited, const QVariantMap& environment = {});

} // namespace Afterglow
