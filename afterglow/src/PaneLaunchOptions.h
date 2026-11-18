// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QStringList>
#include <QVariantMap>
#include <optional>
#include <string>
namespace Afterglow {
struct PaneLaunchOptions
{
    QString cwd;
    QStringList command;
    QVariantMap environment; ///< String values override; false removes inherited entries.
    double fraction = 0.5; ///< New pane's fraction of the split.
    /// The pane stays open after its program exits (afterglow ctl --hold).
    /// Not for a pane of a session, whose program the daemon runs.
    bool hold = false;
    /// The program's standard input: a pipe with these bytes, which then
    /// closes (afterglow ctl --stdin-text). Not for a pane of a session.
    std::optional<std::string> input;
};
}
