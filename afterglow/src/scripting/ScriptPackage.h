// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
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
    static QString read(const QString& manifest, ScriptPackage& result);
};
}
