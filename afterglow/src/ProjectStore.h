// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QCoreApplication>
#include <QJsonObject>
#include <QStringList>

namespace Afterglow {
/// Versioned launch layouts, stored next to the active configuration.
class ProjectStore
{
    Q_DECLARE_TR_FUNCTIONS(ProjectStore)

public:
    explicit ProjectStore(QString directory) : m_directory(std::move(directory)) {}
    static bool validName(const QString& name);
    static QString validate(const QJsonObject& document);
    /// What a pane of a project starts: @p pane is a leaf's "pane" object.
    struct Launch
    {
        QString cwd; ///< absolute and there: a missing one is the home directory
        QStringList command; ///< empty for the shell: also for a command that is not there
        QStringList configured; ///< the command that the project names
    };
    /// The launch of @p pane, with what it had to change in @p warnings.
    static Launch launchOf(const QJsonObject& pane, QStringList& warnings);
    /// The "pane" object of the first pane that a project starts: the first
    /// leaf of its first tab.
    static QJsonObject firstPane(const QJsonObject& document);
    /// The "pane" objects of all the panes of a project, tab by tab, each
    /// tree's first side before its second: the order in which a window
    /// lays them out (TerminalWindow::restoreProject).
    static QList<QJsonObject> panes(const QJsonObject& document);
    /// Whether @p document opens into a mux session ("session": true).
    static bool opensSession(const QJsonObject& document);
    QStringList names() const;
    QString read(const QString& name, QJsonObject& document) const;
    QString save(const QString& name, const QJsonObject& document, bool overwrite) const;
private:
    QString m_directory;
};
} // namespace Afterglow
