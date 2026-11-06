// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../scripting/ScriptHost.h"

#include <AfterglowProto/Run.h>

#include <PhosphorIpc/IpcCallContext.h>
#include <PhosphorIpc/IpcPendingReply.h>
#include <PhosphorIpc/IpcRouter.h>

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QStringList>

#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace Afterglow {

class CommandSocket;
class Control;
class SettingsStore;

/// The calls of the "commands" target: the client side is `aglow run`
/// (AfterglowProto::Run). A run ID is the client's, and belongs to its
/// connection.
///
/// Its events, stdout(run, text) and stderr(run, text), go to the caller of
/// run() only (IpcRouter::sendEvent). The metaobject that CommandSocket
/// builds declares them for the schema, because C++ cannot: stdout and
/// stderr are macros of <cstdio>. A subscription to them gets nothing.
class CommandApi : public QObject, protected PhosphorIpc::IpcCallContext
{
    Q_OBJECT

public:
    explicit CommandApi(CommandSocket& socket);

    /// Each command: {script, command, summary, usage, palette, stdin}.
    Q_INVOKABLE QVariantList list();
    /// Stdin for @p run, before run(), in parts. Kept whole until run(),
    /// cancel() or the close of the connection, and returns true. Input
    /// that would pass Scripts.commandStdinLimit fails with
    /// CommandSocket::InputTooLarge, and the run's input is dropped.
    Q_INVOKABLE bool input(const QString& run, const QString& chunk, bool eof);
    /// Starts @p script's @p command. The reply, deferred until the run ends
    /// and its output has gone, is {exit}. A script or command that does not
    /// exist fails with NO_SUCH_COMMAND. @p pane 0 is the focused pane, and
    /// @p cwd is empty (the pane's directory) or absolute.
    Q_INVOKABLE void run(const QString& run, const QString& script, const QString& command, const QStringList& args,
                         const QString& cwd, double pane);
    /// The client wrote @p bytes of @p run's event lines, '\n' included:
    /// that much more output of the connection's runs can go to it
    /// (CommandSocket::AckWindow). No more than @p run sent counts, so a
    /// client acknowledges each run's count once their total is due.
    Q_INVOKABLE void ack(const QString& run, double bytes);
    /// Drops the output of @p run that waits, and ends the run with status
    /// 130. A run that has ended keeps its own status. For a run that has
    /// not started, its input is dropped.
    Q_INVOKABLE void cancel(const QString& run);

protected:
    /// A call of a command's own entry ("<script>.<command>"): a run with a
    /// run ID of the terminal's, from the focused pane.
    void runEntry(const QString& token, const QStringList& args);

    CommandSocket& m_socket;

private:
    /// Fails the running call with @p code, which sendErrorReply cannot give.
    void fail(const char* code, const QString& message);
};

/// Script commands on the process's socket: the "commands" target, while
/// Control.enabled and Scripts.enabled are on. It keeps each connection's
/// stdin and runs, and cancels the runs of a connection that closes.
///
/// The output of the runs from run() goes out while the client has
/// acknowledged all but AckWindow bytes of it, for all the runs of its
/// connection together. The rest waits here, bounded by
/// Scripts.commandOutputLimit for each run and by MaxWaitingOutputBytes for
/// all. Thus a client that reads slowly holds its runs back, and does not
/// fall behind the router's limit on what waits for a connection, past
/// which the router would close it: for one run at a time (AckWindow).
///
/// The limits here are constants, not settings: they guard the process
/// against clients and scripts, and Scripts.command* already give the user
/// the limits of a run.
class CommandSocket : public QObject
{
    Q_OBJECT

public:
    // The protocol's, which the client has too.
    static constexpr const char* Target = AfterglowProto::Run::Target;
    /// The error code of run() for a script or command that does not exist.
    static constexpr const char* NoSuchCommand = AfterglowProto::Run::NoSuchCommand;
    /// The error code of input() for stdin over Scripts.commandStdinLimit.
    static constexpr const char* InputTooLarge = AfterglowProto::Run::InputTooLarge;
    /// Runs whose stdin waits for run(), per connection: a client that
    /// never calls run() holds no more.
    static constexpr int MaxWaitingInputs = 16;
    /// Stdin that waits for run(), in all connections together, in UTF-8
    /// bytes: MaxWaitingInputs runs at the highest Scripts.commandStdinLimit
    /// (4 MiB). One client can fill it, and more clients cannot add to it.
    static constexpr qint64 MaxWaitingInputBytes = 64 * 1024 * 1024;
    /// Event lines that can go to a connection before its client
    /// acknowledges them: a line goes while less than this waits, so at
    /// most one line more. With a line of at most 6 MiB (each of
    /// EventText's UTF-16 units escaped), the connection stays below the
    /// 16 MiB at which the router closes it, for one run at a time. A run
    /// that ends gives its share back before its client has read its lines,
    /// so the runs after it can pass 16 MiB for a client that reads slowly.
    /// The books of an ended run stay out: a client that forgot them would
    /// stall its other runs, where now the router closes it. aglow run
    /// has one run on its connection.
    static constexpr qint64 AckWindow = qint64(AfterglowProto::Run::AckWindowBytes);
    /// Output that waits for its clients, in all runs together: the UTF-16
    /// text and OutputPartBytes for each part. Scripts.commandOutputLimit
    /// bounds one run in UTF-8 bytes, but many runs, each of many small
    /// parts, could hold far more. A run whose output would pass it ends
    /// with status 1.
    static constexpr qint64 MaxWaitingOutputBytes = 256 * 1024 * 1024;
    /// What one waiting part of output costs besides its text: its place in
    /// the queue, its string and its callback.
    static constexpr qint64 OutputPartBytes = 128;

    CommandSocket(Control& control, ScriptHost& scripts, SettingsStore& settings, PhosphorIpc::IpcRouter& router,
                  QObject* parent = nullptr);
    /// Fails the replies that wait. The runs stay with the scripts, which
    /// end them when they go, after this: without a client, and without a
    /// handler resumed in a process that quits.
    ~CommandSocket() override;

    bool enabled() const { return m_enabled; }
    /// Runs whose stdin waits for run(). A test hook.
    int waitingInputs() const;
    /// The bytes of stdin that wait for run(). A test hook.
    qint64 waitingInputBytes() const { return m_waitingInputBytes; }
    /// What the output that waits for clients costs (MaxWaitingOutputBytes). A test hook.
    qint64 waitingOutputBytes() const { return m_waitingOutputBytes; }
    /// Replaces MaxWaitingOutputBytes. A test hook: a test need not fill 256 MiB.
    void setMaxWaitingOutputBytes(qint64 bytes) { m_maxWaitingOutputBytes = bytes; }

private:
    friend class CommandApi;

    /// Output that waits for the client's acknowledgment.
    struct Output
    {
        const char* event;
        QString text;
        /// After the last part of a write: true once it went, false when it
        /// was dropped. The destructor frees it without a call, as it
        /// resumes no handler in a process that quits (~CommandSocket).
        std::function<void(bool delivered)> sent;
    };

    struct Run
    {
        QByteArray input; ///< UTF-8
        bool hasInput = false;
        bool eof = false;
        bool started = false;
        quint64 handle = 0;
        PhosphorIpc::IpcPendingReply reply;
        bool credit = false; ///< the client acknowledges its output (run())
        qint64 unacknowledged = 0;
        std::deque<Output> output;
        qint64 outputBytes = 0; ///< what output costs toward MaxWaitingOutputBytes
        std::optional<int> exit; ///< the run ended: the reply waits for the output
    };
    using Runs = QHash<QString, std::shared_ptr<Run>>;

    struct Connection
    {
        Runs runs;
        qint64 unacknowledged = 0; ///< of its runs together, for AckWindow
    };

    /// An error of a call: CALL_FAILED when @c code is null.
    struct Refusal
    {
        const char* code = nullptr;
        QString message;
    };

    void applySettings();
    /// Ends every run, whose reply fails with @p message. With @p cancel
    /// the scripts end the runs now, and their output that waits is
    /// dropped. Without, they keep them (the destructor), and the output
    /// that waits goes with no sent() call.
    void dropAll(const QString& message, bool cancel);
    void connectionClosed(quint64 connection);
    std::optional<Refusal> input(quint64 connection, const QString& id, const QString& chunk, bool eof);
    std::optional<Refusal> ack(quint64 connection, const QString& id, double bytes);
    std::optional<Refusal> cancel(quint64 connection, const QString& id);
    /// Starts a run whose reply is @p reply, and answers it when the run cannot start.
    void start(quint64 connection, const QString& id, ScriptHost::CommandRequest request, PhosphorIpc::IpcPendingReply reply,
               bool credit);
    /// Queues @p text as @p event. False when it cannot go, as there is no
    /// such run or it passes MaxWaitingOutputBytes: @p sent is then never
    /// called, and the scripts end the run.
    bool send(quint64 connection, const char* event, const QString& id, const QString& text,
              std::function<void(bool delivered)> sent);
    /// Sends what the client's window allows, then the reply of a run that
    /// ended. False when the client has gone, whose connectionClosed follows.
    bool pump(quint64 connection, const QString& id);
    qint64 unacknowledged(quint64 connection) const;
    /// pump() for each run of @p connection: they share the window.
    void pumpAll(quint64 connection);
    void finish(quint64 connection, const QString& id, int status);
    /// Forgets the run @p id: its stdin, if it did not start, and its share of the window.
    void remove(quint64 connection, const QString& id);
    /// Forgets the stdin of a run that did not start.
    void releaseInput(Run& run);
    /// Takes the output that waits out of @p run's books. Each part's
    /// sent(false) is the caller's to call (dropped()), once the books are
    /// right again: the callback can come back here.
    std::deque<Output> takeOutput(Run& run);
    static void dropped(std::deque<Output>& output);
    static qint64 cost(const Output& output) { return OutputPartBytes + output.text.size() * qint64(sizeof(QChar)); }
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
    QHash<quint64, Connection> m_connections;
    qint64 m_waitingInputBytes = 0;
    qint64 m_waitingOutputBytes = 0;
    qint64 m_maxWaitingOutputBytes = MaxWaitingOutputBytes;
};

} // namespace Afterglow
