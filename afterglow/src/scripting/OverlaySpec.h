// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace Afterglow {

/// What an overlay runs over a pane: the overlay effect of scripts, and the
/// overlay call of the control socket, which share these rules.
struct OverlaySpec
{
    /// Where the program's answer comes from: its standard output, the file
    /// that AFTERGLOW_RESULT names, or nowhere.
    enum class Capture { Stdout, File, None };

    QStringList argv;
    /// Where it starts. Not a local directory (a remote shell's path): $HOME.
    QString cwd;
    /// A string sets the variable, false unsets it.
    QVariantMap env;
    /// Its standard input; unset reads the terminal.
    std::optional<QByteArray> input;
    Capture capture = Capture::Stdout;
    /// Shows as the pane's title while the overlay runs.
    QString title;

    /// Arguments, as for exec.
    static constexpr qsizetype MaxArguments = 64;
    static constexpr qsizetype MaxArgument = 4096;
    static constexpr qsizetype MaxInput = 16 * 1024 * 1024;
    /// Of the captured answer, as for exec.
    static constexpr qsizetype MaxOutput = 1024 * 1024;
    static constexpr qsizetype MaxTitle = 512;

    /// @p argv with @p options (stdin, capture, cwd, env, title), or nothing
    /// with @p error set when one breaks a rule.
    static std::optional<OverlaySpec> read(const QStringList& argv, const QVariantMap& options, QString* error);
};

} // namespace Afterglow
