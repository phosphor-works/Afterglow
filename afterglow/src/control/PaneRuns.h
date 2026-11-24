// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "mcp/DaemonLink.h"

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

namespace Afterglow {

struct Settings;

/// One call of run, wait or send, as a front end (the tools of
/// `afterglow mcp`) hands it to PaneRuns, which answers it once.
class PaneRequest
{
public:
    virtual ~PaneRequest() = default;

    /// The arguments with the names of the tools of `afterglow mcp`: pane,
    /// command, timeout_ms, tail_lines, until, pattern, quiet_ms, text,
    /// paste, keys and signal. A front end checks their types and ranges
    /// against the schemas of those tools before the call.
    virtual QJsonObject arguments() const = 0;
    /// False once the call was answered, or cancelled.
    virtual bool isValid() const = 0;
    /// A result: the structured content, then each text in this order.
    virtual void finish(const QJsonObject& structured, const QStringList& texts) = 0;
    /// A result of one text and no structured content.
    virtual void finishText(const QString& text) = 0;
    virtual void fail(const QString& text) = 0;
    /// Whether the caller takes progress notifications.
    virtual bool wantsProgress() const = 0;
    virtual void progress(double progress, std::optional<double> total, const QString& message) = 0;
    /// Runs @p cancelled once when the caller cancels the call, at once when
    /// it did already. @p cancelled may destroy this object.
    virtual void onCancelled(std::function<void()> cancelled) = 0;
};

/// The settings that the runs and waits read.
struct PaneRunLimits
{
    int waitTimeout = 0; ///< Mcp.waitTimeout
    int maxWaitTimeout = 0; ///< Mcp.maxWaitTimeout
    int tailLines = 0; ///< Mcp.tailLines
    int outputLimit = 0; ///< Mcp.outputLimit
    int eventInterval = 0; ///< Control.eventInterval

    static PaneRunLimits of(const Settings& settings);
};

/// The words of a front end in the errors and hints of PaneRuns: what the
/// caller does next names the front end's own commands. Each field is the
/// text that goes in the place that its comment shows.
struct PaneRunWords
{
    /// Where the panes are: "no pane 3 in WHERE".
    QString where;
    /// Who runs: "pane 3 has a run or a wait until finished of OWNER already".
    QString owner;
    /// "no pane 3 in WHERE: LISTPANES".
    QString listPanes;
    /// "the command did not go: WHY. PANESTATE.", with the pane as %1.
    QString paneState;
    /// A run in "pane 3 has no shell integration: SENDANDWAIT".
    QString sendAndWait;
    /// "pane 3 showed no prompt in 100 ms: its shell has no shell
    /// integration, or has not started. NOPROMPT"
    QString noPrompt;
    /// "pane 3 runs a command: RUNNINGCOMMAND".
    QString runningCommand;
    /// "pane 3 waits for the rest of a command: INTERRUPT".
    QString interrupt;
    /// "pane 3 has text at its prompt that someone typed: TYPEDTEXT".
    QString typedText;
    /// "pane 3 was not ready for the command in 100 ms, and the command did
    /// not go: RUNAGAIN".
    QString runAgain;
    /// "the command is incomplete, and the shell waits for the rest: an open
    /// quote, or a backslash at the end. DROPINCOMPLETE"
    QString dropIncomplete;
    /// The note at the timeout of a run whose shell holds the command: "it
    /// can hold an incomplete command (fish keeps one in its editor).
    /// DROPHELD"
    QString dropHeld;
    /// "Pane 3 still runs its command: WAITFINISHED."
    QString waitFinished;
    /// A wait until finished in "pane 3 has no shell integration: WAITOUTPUT".
    QString waitOutput;
    /// A wait until text with no pattern.
    QString needsPattern;
    /// A wait until quiet with no quiet time.
    QString needsQuietMs;
    /// A send with nothing to send.
    QString needsInput;
    /// A failed call of the daemon that changed nothing, for the caller.
    std::function<QString(const DaemonLink::Error&)> failure;
};

/// The runs, waits and sends in the panes of one session of the mux daemon,
/// through the calls of its control target (docs/MCP.md, the tools run, wait
/// and send). The front end subscribes the link to events() and passes each
/// event to onEvent().
///
/// A run and a wait answer later: PaneRuns keeps the request and ends it at
/// the events of the session (shellChanged, textChanged, paneClosed), at its
/// timeout, or at a cancel. Each check of a wait is one chain of calls at a
/// time: an event that comes meanwhile makes the chain check again when it
/// ends, so no event is lost and no checks pile up.
class PaneRuns : public QObject
{
    Q_OBJECT

public:
    /// The longest message of a progress notification, in characters.
    static constexpr int ProgressMessageLength = 200;

    PaneRuns(DaemonLink& link, const PaneRunLimits& limits, PaneRunWords words, QObject* parent = nullptr);
    ~PaneRuns() override;

    void run(std::unique_ptr<PaneRequest> request);
    void wait(std::unique_ptr<PaneRequest> request);
    void send(std::unique_ptr<PaneRequest> request);

    /// An event of the control target.
    void onEvent(const QString& name, const QJsonArray& args);
    /// Fails every run and wait with @p text: the session is gone.
    void failAll(const QString& text);

    /// The pane object of @p pane in the answer of listPanes, or an empty
    /// one. Notes each pane of @p panes as seen.
    QJsonObject paneIn(const QJsonValue& panes, quint32 pane);
    /// Notes the panes of an answer of listPanes, with their shell states.
    void noteListed(const QJsonArray& panes);
    /// A pane that the front end opened. @p marksPrompts: its shell marks
    /// its prompts, and run and wait wait for the first one.
    void paneOpened(quint32 pane, bool marksPrompts);

private:
    struct Wait;
    /// A run of this front end: where its output starts, and the newest
    /// shell region before it.
    struct RunRecord
    {
        QJsonObject mark;
        qint64 after = 0;
        QElapsedTimer started;
        bool severalLines = false;
    };
    using RequestPtr = std::shared_ptr<PaneRequest>;
    /// The calls of one send: a function and its arguments.
    using SendCalls = QList<std::pair<QString, QJsonArray>>;

    /// The checks of a run before its command goes, from the pane list on.
    void prepareRun(quint64 serial);
    /// The mark and the regions before the command of a run, and its paste.
    void startRun(quint64 serial);
    void pasteCommand(quint64 serial);
    /// The start of a wait from the pane list on.
    void startWait(quint64 serial);
    /// The mark where the text of a wait starts. TRY_AGAIN asks again a
    /// moment later, and startWait waits for it.
    void markWait(quint64 serial);
    /// The mark of the pane before the input of a send, in m_sendMarks,
    /// then the input.
    void markSend(const RequestPtr& request, const std::shared_ptr<const SendCalls>& calls);
    void sendFrom(const RequestPtr& request, const std::shared_ptr<const SendCalls>& calls, qsizetype index);

    /// The wait before a call that a pane answered with TRY_AGAIN goes again.
    int retryMs() const;
    /// Runs @p step after retryMs(), while the wait @p serial waits.
    void retryLater(quint64 serial, std::function<void()> step);
    /// Reads the text of the wait @p serial since its mark, or the text of
    /// the pane's @p fallback scope when a reflow or a clear made the mark
    /// stale, and gives it to @p done. TRY_AGAIN reads again a moment later.
    void readSince(quint64 serial, const QString& fallback,
                   std::function<void(const QJsonObject& answer, const DaemonLink::Error* error, bool stale)> done);
    /// "no pane 3 in session agent: ...".
    QString noPane(quint32 pane) const;
    /// "pane 3 has a run or a wait until finished of ... already".
    QString held(quint32 pane) const;

    /// A new wait for @p request, which it keeps. Its serial, or 0 when the
    /// caller cancelled the request already.
    quint64 addWait(std::unique_ptr<PaneRequest> request, quint32 pane, bool holds);
    Wait* waitOf(quint64 serial);
    void endWait(quint64 serial);
    void finishWait(quint64 serial, const QJsonObject& structured, const QStringList& texts);
    /// Finishes the wait @p serial because its pane closed.
    void finishClosed(quint64 serial, quint32 pane);
    void failWait(quint64 serial, const QString& text);
    /// Starts the timeout of a wait with the time that is left of it.
    void startTimeout(quint64 serial);
    /// Starts the timeout of a wait, and its first check.
    void startWaiting(quint64 serial);
    /// Waits for the first prompt of a pane whose shell starts.
    void awaitPrompt(quint64 serial);
    void check(quint64 serial);
    void checkFinished(quint64 serial);
    void checkText(quint64 serial);
    /// The end of a check that found nothing: another one when an event came meanwhile.
    void checked(quint64 serial);
    /// A check that the pane answered with TRY_AGAIN: another one a moment later.
    void checkLater(quint64 serial);
    /// Fails a run whose command the shell holds unfinished, with what the pane shows.
    void failIncomplete(quint64 serial, const QString& why);
    /// Reads the text since the wait's mark, and finishes it with @p status
    /// and, when not empty, @p note.
    void finishWithText(quint64 serial, const QString& status, const QJsonArray& regions,
                        const QString& note = QString());
    void timedOut(quint64 serial);
    /// The end of a wait until finished at its timeout, from the regions since the command.
    void finishAtTimeout(quint64 serial);
    void progress(quint64 serial);

    DaemonLink& m_link;
    const PaneRunLimits m_limits;
    const PaneRunWords m_words;
    std::map<quint64, std::unique_ptr<Wait>> m_waits;
    quint64 m_nextWait = 0;
    /// The panes with a run, or a wait until finished, of this front end.
    QSet<quint32> m_held;
    /// The last run of each pane, which wait until finished goes on with.
    QHash<quint32, RunRecord> m_runs;
    /// The cursor of each pane before the input of the last send, until a
    /// wait until text reads from it.
    QHash<quint32, QJsonObject> m_sendMarks;
    /// The shell state of each pane, from listPanes and shellChanged.
    QHash<quint32, QString> m_shells;
    /// The panes whose shell marks its prompts and has not marked one yet:
    /// those that this front end started with shell integration.
    QSet<quint32> m_starting;
    /// The panes that the front end heard of: wait until closed tells a pane
    /// that closed from one that never was.
    QSet<quint32> m_seen;
};

} // namespace Afterglow
