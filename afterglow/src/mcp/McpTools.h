// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config/Settings.h"
#include "mcp/DaemonLink.h"
#include "shell/ShellIntegration.h"

#include <PhosphorMcp/McpCall.h>
#include <PhosphorMcp/McpResult.h>
#include <PhosphorMcp/McpTool.h>

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace PhosphorMcp {
class McpServer;
}

namespace Afterglow {

/// The tools of `afterglow mcp` (docs/MCP.md): their definitions, and the
/// handlers that answer them with the calls of the control target of one
/// session of the mux daemon. Each pane argument is a pane ID of the daemon:
/// never the focused pane, which a click in a window that shows the session
/// moves.
///
/// A run and a wait answer later: the handler keeps the call and ends it at
/// the events of the session (shellChanged, textChanged, paneClosed), at its
/// timeout, or at a cancel. Each check of a wait is one chain of calls at a
/// time: an event that comes meanwhile makes the chain check again when it
/// ends, so no event is lost and no checks pile up.
class McpTools : public QObject
{
    Q_OBJECT

public:
    /// The longest text of run, send and paste, in characters. One paste is
    /// one request of the daemon (1 MiB with its JSON escapes): the handlers
    /// refuse a text that does not fit.
    static constexpr int MaxTextLength = 1 << 20;
    /// The longest request line that the server reads: a text of
    /// MaxTextLength that a client writes as JSON escapes of surrogate pairs
    /// (12 bytes for each character), with the rest of a call.
    static constexpr qint64 MaxRequestLineBytes = qint64(MaxTextLength) * 12 + (qint64(1) << 20);
    /// The longest pattern of wait until text, in characters.
    static constexpr int MaxPatternLength = 1024;
    /// The longest message of a progress notification, in characters.
    static constexpr int ProgressMessageLength = 200;

    /// The tools in the order of tools/list. The same at each call: they
    /// depend on no setting.
    static QList<PhosphorMcp::McpTool> definitions();
    /// The events of the control target that the tools follow.
    static QStringList events();

    struct Session
    {
        QString name;
        /// The directory of a new tab that names no pane and no cwd.
        QString directory;
        /// The session's environment, over which the launches of new panes
        /// are prepared (the daemon's selectSession answer).
        std::optional<InheritedEnvironment> environment;
        /// The directory of the user's projects, beside the config file that
        /// the server read: open_project reads there, save_project writes.
        QString projects;
        /// A new connection to the daemon, as a descriptor that the caller
        /// owns, or -1 with @p error: the commands of scripts take a
        /// connection each, which the daemon relays to the window.
        std::function<int(QString* error)> connect;
    };

    McpTools(DaemonLink& link, const Settings& settings, Session session, QObject* parent = nullptr);
    ~McpTools() override;

    /// Adds the tools with their handlers to @p server. False with @p error
    /// when the server refuses one.
    bool addTo(PhosphorMcp::McpServer& server, QString* error);

private:
    struct Wait;
    struct ProjectOpen;
    struct ScriptRun;
    /// A run of this server: where its output starts, and the newest shell
    /// region before it.
    struct RunRecord
    {
        QJsonObject mark;
        qint64 after = 0;
        QElapsedTimer started;
        bool severalLines = false;
    };
    using CallPtr = std::shared_ptr<PhosphorMcp::McpCall>;
    /// The calls of one send: a function and its arguments.
    using SendCalls = QList<std::pair<QString, QJsonArray>>;

    void listPanes(PhosphorMcp::McpCall call);
    void openPane(PhosphorMcp::McpCall call);
    void run(PhosphorMcp::McpCall call);
    void wait(PhosphorMcp::McpCall call);
    void send(PhosphorMcp::McpCall call);
    void sendFrom(const CallPtr& call, const std::shared_ptr<const SendCalls>& calls, qsizetype index);
    void read(PhosphorMcp::McpCall call);
    void screenshot(PhosphorMcp::McpCall call);
    void closePane(PhosphorMcp::McpCall call);
    // The tools of projects and of the commands of scripts, in McpWorkflows.cpp.
    static QList<PhosphorMcp::McpTool> workflowDefinitions();
    void openProject(PhosphorMcp::McpCall call);
    /// The next step of @p open, which ends the call after the last one.
    void openNext(const std::shared_ptr<ProjectOpen>& open);
    /// Closes the panes that @p open made, and fails its call with @p text.
    void failOpen(const std::shared_ptr<ProjectOpen>& open, const QString& text);
    void saveProject(PhosphorMcp::McpCall call);
    /// Writes project @p name from the session's @p layout and the
    /// directories of its panes.
    void writeProject(const CallPtr& call, const QString& name, bool overwrite, const QJsonValue& layout,
                      const std::shared_ptr<QHash<quint32, QString>>& directories);
    void listScriptCommands(PhosphorMcp::McpCall call);
    void runScriptCommand(PhosphorMcp::McpCall call);
    /// A connection of its own that selected the session, for the commands
    /// target, or nullptr with the failed @p call.
    DaemonLink* commandLink(PhosphorMcp::McpCall& call);
    /// Selects the session on @p link, then lists the commands of scripts of
    /// the window that shows it, for @p next. @p failed says why not.
    void listCommands(DaemonLink* link, std::function<void(const QString&)> failed,
                      std::function<void(const QJsonArray&)> next);
    /// A failed call of a link of the commands target, for the model.
    QString commandsFailure(const DaemonLink::Error& error) const;
    void sendInput(const std::shared_ptr<ScriptRun>& run, qsizetype from);
    void startRun(const std::shared_ptr<ScriptRun>& run);
    void finishRun(const std::shared_ptr<ScriptRun>& run, const QJsonValue& value, const DaemonLink::Error* error);
    /// Ends @p run's connection, which ends the run in the window.
    void dropRun(const std::shared_ptr<ScriptRun>& run);
    /// Fails every run of a script command with @p text.
    void endScriptRuns(const QString& text);

    /// The pane object of @p pane in the answer of listPanes, or an empty one.
    static QJsonObject paneIn(const QJsonValue& panes, quint32 pane);
    /// Why the session cannot be used any more, or empty.
    bool ended(PhosphorMcp::McpCall& call) const;

    /// A new wait for @p call, which it keeps. Its serial, or 0 when the
    /// client cancelled the call already.
    quint64 addWait(PhosphorMcp::McpCall call, quint32 pane, bool holds);
    Wait* waitOf(quint64 serial);
    void endWait(quint64 serial);
    void finishWait(quint64 serial, const PhosphorMcp::McpResult& result);
    void failWait(quint64 serial, const QString& text);
    /// Starts the timeout of a wait, and its first check.
    void startWaiting(quint64 serial, int timeoutMs);
    void check(quint64 serial);
    void checkFinished(quint64 serial);
    void checkText(quint64 serial);
    /// The end of a check that found nothing: another one when an event came meanwhile.
    void checked(quint64 serial);
    /// Reads the text since the wait's mark, and finishes it with @p status.
    void finishWithText(quint64 serial, const QString& status, const QJsonArray& regions);
    void timedOut(quint64 serial);
    void progress(quint64 serial);

    void onEvent(const QString& name, const QJsonArray& args);
    /// Ends every wait with @p text, and every later call: the session is gone.
    void endAll(const QString& text);

    DaemonLink& m_link;
    const Settings m_settings;
    const Session m_session;
    std::map<quint64, std::unique_ptr<Wait>> m_waits;
    quint64 m_nextWait = 0;
    /// The panes with a run, or a wait until finished, of this server.
    QSet<quint32> m_held;
    /// The last run of each pane, which wait until finished goes on with.
    QHash<quint32, RunRecord> m_runs;
    /// The shell state of each pane, from listPanes and shellChanged.
    QHash<quint32, QString> m_shells;
    /// The runs of commands of scripts, each with a connection of its own.
    std::map<quint64, std::shared_ptr<ScriptRun>> m_scriptRuns;
    quint64 m_nextScriptRun = 0;
    /// Why the tools cannot reach the session any more.
    QString m_ended;
};

} // namespace Afterglow
