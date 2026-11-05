// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QVariantMap>
#include <QStringList>

namespace Afterglow {
/// A command of a script: `afterglow run ID NAME`, a palette entry, a keybinding.
struct ScriptCommand
{
    QString script; ///< the script's id
    QString name;
    QString summary;
    QString usage; ///< the arguments, for help: "DIR [LAYOUT]"
    QString palette; ///< the palette and menu title, empty when it is in neither
    bool input = false; ///< gets the client's stdin
    bool runtime = false; ///< added by afterglow.set_commands, not the manifest
    QString token() const { return script + QLatin1Char('.') + name; }
    /// The palette title, else "<script> <name>": the title of its notifications.
    QString title() const { return palette.isEmpty() ? script + QLatin1Char(' ') + name : palette; }
    bool operator==(const ScriptCommand&) const = default;
};
/// The limits of the commands field (package.json, metadata) and of set_commands.
inline constexpr int MaxScriptCommands = 64;
inline constexpr int MaxCommandText = 512; ///< UTF-16 units of a summary, usage or palette title
/// Checks one command's fields (summary, usage, palette, stdin) and fills
/// @p out. Returns an error, or empty.
QString readCommand(const QString& name, const QVariantMap& fields, ScriptCommand& out);
/// Checks a commands table, name to fields. Returns an error, or empty.
QString readCommands(const QVariant& table, QList<ScriptCommand>& out);

/// A bounded, local package snapshot. Modules and vendored dependencies run in one sandbox.
struct ScriptPackage
{
    QString id;
    QString version;
    int stateVersion = 1;
    QVariantMap config;
    QVariantMap assets;
    QByteArray source;
    QStringList watched;
    /// Shader packs the package ships, each declaring the id "<package>.<pack>".
    QString shaderDirectory;
    QStringList shaderPacks;
    /// Shader settings it suggests: a Shaders key and one of its pack names.
    QVariantMap suggests;
    /// Pulse effects its packs draw, by name: afterglow.pulse(pane, name).
    QHash<QString, int> pulses;
    /// Its commands (package.json "commands"), without the script id.
    QList<ScriptCommand> commands;
    /// Where each module's source starts in the generated chunk, for error positions.
    struct Module
    {
        int line;
        int lines;
        QString file; ///< relative to the package directory
    };
    QList<Module> modules;
    static QString read(const QString& manifest, ScriptPackage& result);
    /// @p message with positions in the generated chunk named by module file and line.
    static QString locate(const QList<Module>& modules, const QString& message);
};
}
