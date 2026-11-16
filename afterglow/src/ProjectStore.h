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
    /// The most arguments of a pane command, and the most UTF-8 bytes of one
    /// argument, that a project keeps: those that a session's layout keeps
    /// too (sessionCommand).
    static constexpr qsizetype MaxCommandArgs = 64;
    static constexpr qsizetype MaxCommandArgBytes = 4096;
    /// The most UTF-16 units of a tab or pane title that a project keeps.
    static constexpr qsizetype MaxTitle = 256;
    /// Whether a project keeps @p command as a pane's command: a program
    /// name first, no NUL, and within MaxCommandArgs and MaxCommandArgBytes.
    /// The empty command (the shell) fits.
    static bool keepsCommand(const QStringList& command);
    /// What a pane of a project starts: @p pane is a leaf's "pane" object.
    struct Launch
    {
        /// Absolute. launchOf found it a directory, else it is the home
        /// directory, which launchOf does not check.
        QString cwd;
        QStringList command; ///< empty for the shell: also for a command that is not there
        QStringList configured; ///< the command that the project names
    };
    /// The launch of @p pane, with what it had to change in @p warnings. A
    /// program without a slash is looked for in @p searchPath, the PATH that
    /// the pane's program gets, split into its directories (launchSearchPath),
    /// a relative one in the pane's directory: empty for the PATH of this
    /// process.
    static Launch launchOf(const QJsonObject& pane, QStringList& warnings, const QStringList& searchPath = {});
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
