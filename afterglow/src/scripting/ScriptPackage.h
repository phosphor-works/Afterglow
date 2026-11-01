// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QVariantMap>
#include <QStringList>

namespace Afterglow {
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
