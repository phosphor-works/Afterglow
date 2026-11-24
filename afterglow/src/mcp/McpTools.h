// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config/Settings.h"
#include "control/PaneRuns.h"
#include "mcp/DaemonLink.h"
#include "shell/ShellIntegration.h"

#include <PhosphorMcp/McpCall.h>
#include <PhosphorMcp/McpResult.h>
#include <PhosphorMcp/McpTool.h>

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

namespace McpParts {
struct ProjectStep;
}

/// The tools of `afterglow mcp` (docs/MCP.md): their definitions, and the
/// handlers that answer them with the calls of the control target of one
/// session of the mux daemon. Each pane argument is a pane ID of the daemon:
/// never the focused pane, which a click in a window that shows the session
/// moves.
///
/// run, wait and send hand their calls to PaneRuns, which answers them
/// later, at the events of the session.
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
        /// owns, or -1 with @p error, and @p noDaemon (when not null) true
        /// when no daemon listens: the commands of scripts take a connection
        /// each, which the daemon relays to the window.
        std::function<int(QString* error, bool* noDaemon)> connect;
        /// The first pane of a session that the server made, when its launch
        /// starts a shell that marks its prompts, else 0: run and wait wait
        /// for its first prompt.
        quint32 startingPane = 0;
        /// The daemon bound the session to the link: it ends with the
        /// server, unless another server binds it too. When the link
        /// closes, the server binds it on a new connection (connect) and
        /// closes that: the daemon lets a session run on whose connection it
        /// closed because the server read nothing, and the server cannot
        /// use it any more. Only on a connection to the same daemon process
        /// (DaemonLink::peer): a later daemon may hold another session of
        /// the same name.
        bool endsWithServer = false;
    };

    McpTools(DaemonLink& link, const Settings& settings, Session session, QObject* parent = nullptr);
    ~McpTools() override;

    /// Adds the tools with their handlers to @p server. False with @p error
    /// when the server refuses one.
    bool addTo(PhosphorMcp::McpServer& server, QString* error);

private:
    struct ProjectOpen;
    struct ScriptRun;
    using CallPtr = std::shared_ptr<PhosphorMcp::McpCall>;

    void listPanes(PhosphorMcp::McpCall call);
    void openPane(PhosphorMcp::McpCall call);
    void run(PhosphorMcp::McpCall call);
    void wait(PhosphorMcp::McpCall call);
    void send(PhosphorMcp::McpCall call);
    void read(PhosphorMcp::McpCall call);
    void screenshot(PhosphorMcp::McpCall call);
    void closePane(PhosphorMcp::McpCall call);
    // The tools of projects and of the commands of scripts, in McpWorkflows.cpp.
    static QList<PhosphorMcp::McpTool> workflowDefinitions();
    void openProject(PhosphorMcp::McpCall call);
    /// The next step of @p open, which ends the call after the last one.
    void openNext(const std::shared_ptr<ProjectOpen>& open);
    /// Closes the panes that @p open made, and fails its call with @p text.
    /// A close never ends the session: the last pane of the session stays,
    /// and the text says so.
    void failOpen(const std::shared_ptr<ProjectOpen>& open, const QString& text);
    /// failOpen for the @p error of the call that opens the pane of @p step.
    void failStep(const std::shared_ptr<ProjectOpen>& open, const McpParts::ProjectStep& step,
                  const DaemonLink::Error& error);
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
    void startScriptRun(const std::shared_ptr<ScriptRun>& run);
    void finishRun(const std::shared_ptr<ScriptRun>& run, const QJsonValue& value, const DaemonLink::Error* error);
    /// Ends @p run's connection, which ends the run in the window.
    void dropRun(const std::shared_ptr<ScriptRun>& run);
    /// Fails every run of a script command with @p text.
    void endScriptRuns(const QString& text);

    /// Fails @p call with why the session cannot be used any more, if it cannot.
    bool ended(PhosphorMcp::McpCall& call) const;

    void onEvent(const QString& name, const QJsonArray& args);
    /// The link to the daemon closed: lets the daemon end the session first
    /// when it ends with the server (Session::endsWithServer) and the same
    /// daemon still runs, then ends every wait.
    void linkClosed(const QString& reason);
    /// Ends every wait with @p text, and every later call: the session is gone.
    void endAll(const QString& text);

    DaemonLink& m_link;
    const Settings m_settings;
    const Session m_session;
    /// The runs, waits and sends of the tools run, wait and send.
    PaneRuns m_paneRuns;
    /// The panes that closed, for the splits of an open project.
    QSet<quint32> m_closed;
    /// The runs of commands of scripts, each with a connection of its own.
    std::map<quint64, std::shared_ptr<ScriptRun>> m_scriptRuns;
    quint64 m_nextScriptRun = 0;
    /// Why the tools cannot reach the session any more, or empty.
    QString m_ended;
};

} // namespace Afterglow
