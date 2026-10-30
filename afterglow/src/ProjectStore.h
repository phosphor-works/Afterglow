// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonObject>
#include <QStringList>

namespace Afterglow {
/// Versioned launch layouts, stored next to the active configuration.
class ProjectStore
{
public:
    explicit ProjectStore(QString directory) : m_directory(std::move(directory)) {}
    static bool validName(const QString& name);
    static QString validate(const QJsonObject& document);
    QStringList names() const;
    QString read(const QString& name, QJsonObject& document) const;
    QString save(const QString& name, const QJsonObject& document, bool overwrite) const;
private:
    QString m_directory;
};
} // namespace Afterglow
