// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../scripting/ScriptHost.h"

#include <PhosphorIpc/IpcCallContext.h>
#include <PhosphorIpc/IpcPendingReply.h>
#include <PhosphorIpc/IpcRouter.h>

#include <QHash>
#include <QObject>
#include <QStringList>

#include <memory>
#include <optional>

namespace Afterglow {

class CommandSocket;
class Control;
class SettingsStore;

/// The calls of the "commands" target: the client side is `afterglow run`
/// (AfterglowProto::Run). A run ID is the client's, and belongs to its
/// connection. The events go to the caller only.
class CommandApi : public QObject, protected PhosphorIpc::IpcCallContext
{
    Q_OBJECT

public:
    explicit CommandApi(CommandSocket& socket);

    /// Each command: {script, command, summary, usage, palette, stdin}.
    Q_INVOKABLE QVariantList list();
    /// Stdin for @p run, before run(). Kept until run() or the close of the
    /// connection, cut at Scripts.commandStdinLimit. Returns whether the
    /// terminal takes more: false after @p eof and at the limit.
    Q_INVOKABLE bool input(const QString& run, const QString& chunk, bool eof);
    /// Starts @p script's @p command. The reply, deferred until the run ends,
    /// is {exit}. A script or command that does not exist fails with
    /// NO_SUCH_COMMAND. @p pane 0 is the focused pane.
    Q_INVOKABLE void run(const QString& run, const QString& script, const QString& command, const QStringList& args,
                         const QString& cwd, double pane);
    Q_INVOKABLE void cancel(const QString& run);

Q_SIGNALS:
    // Sent to the caller of run() only (IpcRouter::sendEvent). Declared for
    // the schema: a subscription to them gets nothing.
    void stdout(const QString& run, const QString& text);
    void stderr(const QString& run, const QString& text);

protected:
    /// A call of a command's own entry ("<script>.<command>"): a run with a
    /// run ID of the terminal's, from the focused pane.
    void runEntry(const QString& token, const QStringList& args);

    CommandSocket& m_socket;
};

/// Script commands on the process's socket: the "commands" target, while
/// Control.enabled and Scripts.enabled are on. It keeps each connection's
/// stdin and runs, and cancels the runs of a connection that closes.
class CommandSocket : public QObject
{
    Q_OBJECT

public:
    static constexpr const char* Target = "commands";
    /// The error code of run() for a script or command that does not exist.
    static constexpr const char* NoSuchCommand = "NO_SUCH_COMMAND";
    /// Stdin that waits for its run(), per connection.
    static constexpr int MaxWaitingInputs = 16;

    CommandSocket(Control& control, ScriptHost& scripts, SettingsStore& settings, PhosphorIpc::IpcRouter& router,
                  QObject* parent = nullptr);
    ~CommandSocket() override;

    bool enabled() const { return m_enabled; }
    /// Runs whose stdin waits for run(). A test hook.
    int waitingInputs() const;

private:
    friend class CommandApi;

    struct Run
    {
        QString input;
        qint64 inputBytes = 0;
        bool hasInput = false;
        bool eof = false;
        bool started = false;
        quint64 handle = 0;
        PhosphorIpc::IpcPendingReply reply;
    };
    using Runs = QHash<QString, std::shared_ptr<Run>>;

    void applySettings();
    /// Ends every run, whose reply fails with @p message.
    void dropAll(const QString& message);
    void connectionClosed(quint64 connection);
    /// Starts a run whose reply is @p reply, and answers it when the run cannot start.
    void start(quint64 connection, const QString& id, ScriptHost::CommandRequest request, PhosphorIpc::IpcPendingReply reply);
    bool send(quint64 connection, const char* event, const QString& id, const QString& text);
    void finish(quint64 connection, const QString& id, int status);
    std::shared_ptr<Run> find(quint64 connection, const QString& id) const;
    /// The pane of a call: @p pane, or for 0 the focused pane, 0 when there
    /// is none. Empty with @p error.
    std::optional<quint32> pane(double pane, QString& error) const;

    Control& m_control;
    ScriptHost& m_scripts;
    SettingsStore& m_settings;
    PhosphorIpc::IpcRouter& m_router;
    std::unique_ptr<CommandApi> m_api;
    bool m_enabled = false;
    QHash<quint64, Runs> m_runs;
};

} // namespace Afterglow
