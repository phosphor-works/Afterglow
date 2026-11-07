// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace Afterglow {

/// Environment overrides of a program that a script or a control client
/// starts: exec, launch options and overlays.
struct EnvironmentOverrides
{
    static constexpr qsizetype MaxVariables = 256;
    /// UTF-8 of the names and the values together.
    static constexpr qsizetype MaxBytes = 64 * 1024;

    /// @p value as variables in @p env (a string sets one, false unsets it),
    /// or an error. Nothing, and an empty list (an empty Luau table), are no
    /// overrides.
    static QString read(const QVariant& value, QVariantMap& env);
};

/// What an overlay runs over a pane: the overlay effect of scripts, and the
/// overlay call of the control socket, which share these rules.
struct OverlaySpec
{
    /// Where the program's answer comes from: its standard output, the file
    /// that AFTERGLOW_RESULT names, or nowhere.
    enum class Capture { Stdout, File, None };

    QStringList argv;
    /// Empty, absolute, ~ or ~/...: where it starts. Not a local directory
    /// (a remote shell's path): $HOME.
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
    /// UTF-8 bytes. A script builds the text in its VM of 16 MiB, beside the
    /// list that it joins: more could not reach here.
    static constexpr qsizetype MaxInput = 4 * 1024 * 1024;
    /// Of the captured answer, as for exec.
    static constexpr qsizetype MaxOutput = 1024 * 1024;
    static constexpr qsizetype MaxTitle = 512;

    /// @p argv with @p options (stdin, capture, cwd, env, title), or nothing
    /// with @p error set when one breaks a rule. An option that is not
    /// listed is refused, not dropped.
    static std::optional<OverlaySpec> read(const QStringList& argv, const QVariantMap& options, QString* error);
};

} // namespace Afterglow

// ScriptHost hands the spec that it read to ScriptControl inside the effect.
Q_DECLARE_METATYPE(Afterglow::OverlaySpec)
