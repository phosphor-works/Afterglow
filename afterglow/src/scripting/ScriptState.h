// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QVariantMap>
namespace Afterglow {
class ScriptState
{
public:
    explicit ScriptState(QString directory) : m_directory(std::move(directory)) {}
    struct Snapshot { int version = 0; int revision = 0; QVariantMap value; };
    QString read(const QString& id, Snapshot& result) const;
    QString write(const QString& id, int version, int expectedRevision, const QVariantMap& value, Snapshot& result) const;
    static QString validate(const QVariantMap& value);
private:
    QString m_directory;
};
}
