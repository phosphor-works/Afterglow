// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/PaneRuns.h"

#include "config/Settings.h"
#include "mcp/McpToolParts.h"

#include "MuxProtocol.h"

#include <QDeadlineTimer>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Afterglow {

namespace {

using namespace McpParts;

/// The shortest wait before a call that a pane answered with TRY_AGAIN goes
/// again, also with Control.eventInterval 0: the pane decodes an image for a
/// while, and a call at each turn of the event loop would only load it.
constexpr int MinRetryMs = 20;

quint32 paneOf(const PaneRequest& request)
{
    return quint32(request.arguments().value(L("pane")).toInteger());
}

/// The options of the paste of a run: only at an empty prompt, and Enter in
/// the same write, so that nothing comes between the two.
QJsonObject runPaste()
{
    return {{L("prompt"), true}, {L("enter"), true}};
}

/// What a part of a send is, by its function.
QString sendPart(const QString& fn)
{
    if (fn == L("sendText"))
        return QStringLiteral("text");
    if (fn == L("paste"))
        return QStringLiteral("paste");
    if (fn == L("sendKeys"))
        return QStringLiteral("keys");
    return QStringLiteral("signal");
}

} // namespace

PaneRunLimits PaneRunLimits::of(const Settings& settings)
{
    return {.waitTimeout = settings.mcp.waitTimeout,
            .maxWaitTimeout = settings.mcp.maxWaitTimeout,
            .tailLines = settings.mcp.tailLines,
            .outputLimit = settings.mcp.outputLimit,
            .eventInterval = settings.control.eventInterval};
}

struct PaneRuns::Wait
{
    enum class Until {
        Finished,
        Text,
        Quiet,
        Closed,
    };
    quint64 serial = 0;
    std::unique_ptr<PaneRequest> request;
    bool run = false; ///< a run, not a wait
    Until until = Until::Finished;
    quint32 pane = 0;
    bool holds = false; ///< m_held has the pane for it
    bool ready = false; ///< the events check it
    /// The shell of the pane starts: the call goes on at its first prompt.
    bool awaitingPrompt = false;
    /// The end of the call's time, from its start.
    QDeadlineTimer deadline;
    int timeoutMs = 0;
    QString command; ///< of a run
    /// Where the output starts, and the newest region before it.
    QJsonObject mark;
    qint64 after = 0;
    QElapsedTimer started; ///< of the run, invalid for none
    bool severalLines = false; ///< the command of the run has a line end
    int tailLines = 0;
    QTimer timeout;
    QTimer quiet;
    int quietMs = 0;
    QRegularExpression pattern;
    /// Where the next check of until text reads from: the start of the
    /// cursor's line at the check before.
    QJsonObject searchMark;
    /// The mark of a send before the wait: no check reads before it, where
    /// the line in progress still showed the prompt that the input answered.
    QJsonObject floor;
    /// Why the mark at the start of a wait failed.
    QString markError;
    /// The mark at the start of a wait has not come: a pane that decodes an
    /// image answers it a moment later (markWait).
    bool markPending = false;
    /// The pane list came before the mark: startWait goes on when it comes.
    bool startAfterMark = false;
    bool checking = false;
    bool again = false;
    /// The timeout came: a check that starts now is the last one.
    bool lastCheck = false;
    bool lastCheckStarted = false;
    qint64 progressBase = 0;
    qint64 progressEpoch = 0;
    double progressSent = 0;
    bool progressBusy = false;
    bool progressAgain = false;
};

PaneRuns::PaneRuns(DaemonLink& link, const PaneRunLimits& limits, PaneRunWords words, QObject* parent)
    : QObject(parent)
    , m_link(link)
    , m_limits(limits)
    , m_words(std::move(words))
{
}

PaneRuns::~PaneRuns() = default;

int PaneRuns::retryMs() const
{
    return std::max(m_limits.eventInterval, MinRetryMs);
}

void PaneRuns::retryLater(quint64 serial, std::function<void()> step)
{
    QTimer::singleShot(retryMs(), this, [this, serial, step = std::move(step)] {
        if (waitOf(serial))
            step();
    });
}

QString PaneRuns::noPane(quint32 pane) const
{
    return QStringLiteral("no pane %1 in %2: %3").arg(QString::number(pane), m_words.where, m_words.listPanes);
}

QString PaneRuns::held(quint32 pane) const
{
    return QStringLiteral("pane %1 has a run or a wait until finished of %2 already: wait for it")
        .arg(QString::number(pane), m_words.owner);
}

QJsonObject PaneRuns::paneIn(const QJsonValue& panes, quint32 pane)
{
    QJsonObject found;
    for (const QJsonValue& value : panes.toArray()) {
        const quint32 id = quint32(value.toObject().value(L("id")).toInteger());
        m_seen.insert(id);
        if (id == pane)
            found = value.toObject();
    }
    return found;
}

void PaneRuns::noteListed(const QJsonArray& panes)
{
    for (const QJsonValue& item : panes) {
        const QJsonObject pane = item.toObject();
        const quint32 id = quint32(pane.value(L("id")).toInteger());
        const QString shell = pane.value(L("shell")).toString();
        m_seen.insert(id);
        m_shells.insert(id, shell);
        if (shell != L("none"))
            m_starting.remove(id);
    }
}

void PaneRuns::paneOpened(quint32 pane, bool marksPrompts)
{
    m_seen.insert(pane);
    if (marksPrompts)
        m_starting.insert(pane);
}

quint64 PaneRuns::addWait(std::unique_ptr<PaneRequest> request, quint32 pane, bool holds)
{
    const quint64 serial = ++m_nextWait;
    auto wait = std::make_unique<Wait>();
    wait->serial = serial;
    wait->request = std::move(request);
    wait->pane = pane;
    wait->holds = holds;
    wait->tailLines = m_limits.tailLines;
    wait->timeout.setSingleShot(true);
    wait->quiet.setSingleShot(true);
    // A coarse timer may fire 5% early: the quiet time is a least, which an
    // early end would cut below two event intervals.
    wait->quiet.setTimerType(Qt::PreciseTimer);
    connect(&wait->timeout, &QTimer::timeout, this, [this, serial] {
        timedOut(serial);
    });
    if (holds)
        m_held.insert(pane);
    Wait& added = *m_waits.emplace(serial, std::move(wait)).first->second;
    // A call that the client cancelled already ends here, at once.
    added.request->onCancelled([this, serial] {
        endWait(serial);
    });
    return waitOf(serial) ? serial : 0;
}

PaneRuns::Wait* PaneRuns::waitOf(quint64 serial)
{
    const auto it = m_waits.find(serial);
    return it == m_waits.end() ? nullptr : it->second.get();
}

void PaneRuns::endWait(quint64 serial)
{
    const auto it = m_waits.find(serial);
    if (it == m_waits.end())
        return;
    if (it->second->holds)
        m_held.remove(it->second->pane);
    // Also from the cancel callback of its request, which may destroy the request.
    m_waits.erase(it);
}

void PaneRuns::finishWait(quint64 serial, const QJsonObject& structured, const QStringList& texts)
{
    if (Wait* wait = waitOf(serial)) {
        wait->request->finish(structured, texts);
        endWait(serial);
    }
}

void PaneRuns::finishClosed(quint64 serial, quint32 pane)
{
    finishWait(serial,
               QJsonObject{{L("status"), L("closed")},
                           {L("matched"), true},
                           {L("match"), QJsonValue()},
                           {L("exit_code"), QJsonValue()},
                           {L("exit_codes"), QJsonArray()},
                           {L("duration_ms"), QJsonValue()},
                           {L("output"), QString()},
                           {L("truncated"), false},
                           {L("notes"), QJsonArray()}},
               {QStringLiteral("Pane %1 is closed.").arg(pane)});
}

void PaneRuns::failWait(quint64 serial, const QString& text)
{
    if (Wait* wait = waitOf(serial)) {
        wait->request->fail(text);
        endWait(serial);
    }
}

void PaneRuns::startTimeout(quint64 serial)
{
    Wait* wait = waitOf(serial);
    if (!wait)
        return;
    wait->timeout.start(int(std::clamp<qint64>(wait->deadline.remainingTime(), 1, std::numeric_limits<int>::max())));
}

void PaneRuns::startWaiting(quint64 serial)
{
    Wait* wait = waitOf(serial);
    if (!wait)
        return;
    wait->ready = true;
    startTimeout(serial);
    if (wait->until == Wait::Until::Quiet)
        return;
    // Also at the start: an event that came before the wait would be lost.
    check(serial);
}

void PaneRuns::awaitPrompt(quint64 serial)
{
    Wait* wait = waitOf(serial);
    if (!wait)
        return;
    // The next shellChanged of the pane goes on (onEvent), or the timeout fails it.
    wait->awaitingPrompt = true;
    startTimeout(serial);
}

void PaneRuns::check(quint64 serial)
{
    Wait* wait = waitOf(serial);
    if (!wait || !wait->ready)
        return;
    if (wait->checking) {
        wait->again = true;
        return;
    }
    wait->checking = true;
    wait->again = false;
    if (wait->lastCheck)
        wait->lastCheckStarted = true;
    if (wait->until == Wait::Until::Finished)
        checkFinished(serial);
    else if (wait->until == Wait::Until::Text)
        checkText(serial);
    else
        checked(serial);
}

void PaneRuns::checked(quint64 serial)
{
    Wait* wait = waitOf(serial);
    if (!wait)
        return;
    wait->checking = false;
    if (wait->again)
        check(serial);
}

void PaneRuns::checkLater(quint64 serial)
{
    Wait* wait = waitOf(serial);
    if (!wait)
        return;
    // Found nothing yet: the pane answers a moment later. A command that
    // went is never sent again.
    wait->checking = false;
    retryLater(serial, [this, serial] {
        check(serial);
    });
}

void PaneRuns::checkFinished(quint64 serial)
{
    const Wait& wait = *waitOf(serial);
    m_link.call(QStringLiteral("shellRegions"), {qint64(wait.pane), wait.after},
                [this, serial](const QJsonValue& value, const DaemonLink::Error* error) {
                    Wait* wait = waitOf(serial);
                    // The timeout took the wait over.
                    if (!wait || !wait->ready)
                        return;
                    if (tryAgain(error))
                        return checkLater(serial);
                    if (error)
                        return failWait(serial, m_words.failure(*error));
                    const QJsonArray regions = value.toArray();
                    // A new prompt after the command: it ended, and so did each
                    // line of a paste of many lines, or nothing ran (an empty
                    // line, a comment). A secondary prompt makes no region.
                    if (commandEnded(regions, wait->severalLines))
                        return finishWithText(serial, QStringLiteral("finished"), regions);
                    if (m_shells.value(wait->pane) == L("continuation")) {
                        // Nothing more comes: the shell waits for the rest.
                        return failIncomplete(serial,
                                              QStringLiteral("the command is incomplete, and the shell waits for the "
                                                             "rest: an open quote, or a backslash at the end. %1")
                                                  .arg(m_words.dropIncomplete));
                    }
                    checked(serial);
                });
}

void PaneRuns::failIncomplete(quint64 serial, const QString& why)
{
    Wait* wait = waitOf(serial);
    if (!wait)
        return;
    wait->ready = false;
    wait->timeout.stop();
    m_runs.remove(wait->pane);
    // The command and the secondary prompt: a reflow or a clear leaves the screen.
    readSince(serial, QStringLiteral("screen"),
              [this, serial, why](const QJsonObject& answer, const DaemonLink::Error* error, bool) {
                  Wait* wait = waitOf(serial);
                  if (!wait)
                      return;
                  if (error)
                      return failWait(serial, why);
                  const Output output =
                      shape(answer.value(L("text")).toString(), wait->tailLines, m_limits.outputLimit, false);
                  failWait(serial, QStringLiteral("%1 The pane shows:\n%2").arg(why, output.text));
              });
}

void PaneRuns::checkText(quint64 serial)
{
    const Wait& wait = *waitOf(serial);
    // The cursor's row first, then the text: what comes between the two is
    // read again at the next check, never skipped.
    m_link.call(
        QStringLiteral("mark"), {qint64(wait.pane)},
        [this, serial](const QJsonValue& value, const DaemonLink::Error* error) {
            Wait* wait = waitOf(serial);
            if (!wait || !wait->ready)
                return;
            if (tryAgain(error))
                return checkLater(serial);
            if (error)
                return failWait(serial, m_words.failure(*error));
            const QJsonObject mark = value.toObject();
            const qint64 row = mark.value(L("row")).toInteger();
            const qint64 epoch = epochOf(mark);
            // A reflow or a clear gave the rows of the search mark other
            // text: the screen holds what came last.
            const QJsonArray args = epochOf(wait->searchMark) == epoch && !wait->searchMark.isEmpty()
                ? textArgs(wait->pane, QStringLiteral("since"), {{L("mark"), wait->searchMark}})
                : textArgs(wait->pane, QStringLiteral("screen"), keptEnd());
            m_link.call(
                QStringLiteral("getText"), args,
                [this, serial, row, epoch](const QJsonValue& value, const DaemonLink::Error* error) {
                    Wait* wait = waitOf(serial);
                    if (!wait || !wait->ready)
                        return;
                    if (tryAgain(error))
                        return checkLater(serial);
                    if (hasCode(error, AfterglowMux::Error::StaleMark)) {
                        // A reflow or a clear between the mark and the
                        // text: the next check reads the screen.
                        wait->searchMark = rowMark(row, -1);
                        wait->again = true;
                        return checked(serial);
                    }
                    if (error)
                        return failWait(serial, m_words.failure(*error));
                    const QJsonObject answer = value.toObject();
                    const QString text = answer.value(L("text")).toString();
                    const QRegularExpressionMatch match = wait->pattern.match(text);
                    if (match.hasMatch()) {
                        const Output output =
                            shape(text, wait->tailLines, m_limits.outputLimit, answer.value(L("truncated")).toBool());
                        const Output matched = shape(match.captured(0), 0, m_limits.outputLimit, false);
                        QJsonObject result{
                            {L("status"), L("matched")},     {L("matched"), true},
                            {L("match"), matched.text},      {L("exit_code"), QJsonValue()},
                            {L("exit_codes"), QJsonArray()}, {L("duration_ms"), QJsonValue()},
                            {L("output"), output.text},      {L("truncated"), output.truncated || matched.truncated},
                            {L("notes"), QJsonArray()}};
                        return finishWait(serial, result, {QStringLiteral("Pane %1 matched.").arg(wait->pane)});
                    }
                    // The next check reads from the start of the cursor's
                    // line: the line in progress, and what comes after it.
                    // On the row of a send, from where the input went.
                    const QJsonObject& floor = wait->floor;
                    wait->searchMark =
                        !floor.isEmpty() && epochOf(floor) == epoch && floor.value(L("row")).toInteger() == row
                        ? floor
                        : rowMark(row, epoch);
                    if (wait->lastCheckStarted)
                        return finishWithText(serial, QStringLiteral("running"), {});
                    checked(serial);
                });
        });
}

void PaneRuns::readSince(
    quint64 serial, const QString& fallback,
    std::function<void(const QJsonObject& answer, const DaemonLink::Error* error, bool stale)> done)
{
    const Wait& wait = *waitOf(serial);
    const quint32 pane = wait.pane;
    m_link.call(QStringLiteral("getText"), textArgs(pane, QStringLiteral("since"), {{L("mark"), wait.mark}}),
                [this, serial, pane, fallback, done](const QJsonValue& value, const DaemonLink::Error* error) {
                    if (!waitOf(serial))
                        return;
                    if (tryAgain(error)) {
                        return retryLater(serial, [this, serial, fallback, done] {
                            readSince(serial, fallback, done);
                        });
                    }
                    if (!hasCode(error, AfterglowMux::Error::StaleMark))
                        return done(value.toObject(), error, false);
                    // The pane changed its width or cleared its screen since the
                    // mark: its rows hold other text.
                    m_link.call(
                        QStringLiteral("getText"),
                        textArgs(pane, fallback, fallback == L("screen") ? keptEnd() : QJsonObject()),
                        [this, serial, fallback, done](const QJsonValue& value, const DaemonLink::Error* error) {
                            if (!waitOf(serial))
                                return;
                            if (tryAgain(error)) {
                                return retryLater(serial, [this, serial, fallback, done] {
                                    readSince(serial, fallback, done);
                                });
                            }
                            done(value.toObject(), error, true);
                        });
                });
}

void PaneRuns::finishWithText(quint64 serial, const QString& status, const QJsonArray& regions, const QString& note)
{
    Wait* wait = waitOf(serial);
    if (!wait)
        return;
    // One reply for each call: the end comes once.
    wait->ready = false;
    wait->timeout.stop();
    wait->quiet.stop();
    readSince(
        serial, QStringLiteral("last-output"),
        [this, serial, status, regions, note](const QJsonObject& answer, const DaemonLink::Error* error, bool stale) {
            Wait* wait = waitOf(serial);
            if (!wait)
                return;
            if (error && error->code.isEmpty())
                return failWait(serial, m_words.failure(*error));
            // The command ended all the same: its exit codes count
            // without its text.
            const Output output = error
                ? Output{QString(), true}
                : shape(answer.value(L("text")).toString(), wait->tailLines, m_limits.outputLimit,
                        stale || answer.value(L("truncated")).toBool() || answer.value(L("trimmed")).toBool());
            const QJsonArray codes = exitCodes(regions);
            const bool finished = status == L("finished");
            if (finished)
                m_runs.remove(wait->pane);
            QJsonObject result{
                {L("status"), status},
                {L("exit_code"), finished && !codes.isEmpty() ? codes.last() : QJsonValue()},
                {L("exit_codes"), codes},
                {L("duration_ms"), wait->started.isValid() ? QJsonValue(wait->started.elapsed()) : QJsonValue()},
                {L("output"), output.text},
                {L("truncated"), output.truncated},
            };
            if (!wait->run) {
                result.insert(L("matched"), status != L("running"));
                result.insert(L("match"), QJsonValue());
            }
            // A client that reads only the structured result reads these too.
            QStringList notes;
            if (!note.isEmpty())
                notes << note;
            if (stale) {
                notes << QStringLiteral(
                    "The pane changed its width or cleared its screen since the wait began: "
                    "output is the output of the last command only.");
            }
            if (error)
                notes << QStringLiteral("The output could not be read: %1").arg(error->message);
            result.insert(L("notes"), QJsonArray::fromStringList(notes));
            QStringList texts;
            if (status == L("running")) {
                texts << QStringLiteral("Pane %1 still runs its command: %2.")
                             .arg(QString::number(wait->pane), m_words.waitFinished);
            } else if (finished) {
                texts << QStringLiteral("The command in pane %1 finished.").arg(wait->pane);
            } else {
                texts << QStringLiteral("Pane %1: %2.").arg(wait->pane).arg(status);
            }
            texts << notes;
            finishWait(serial, result, texts);
        });
}

void PaneRuns::timedOut(quint64 serial)
{
    Wait* wait = waitOf(serial);
    if (!wait)
        return;
    if (wait->awaitingPrompt) {
        // A shell that showed no prompt in the whole time of a call shows
        // none soon: the next call does not wait for one again.
        m_starting.remove(wait->pane);
        return failWait(serial,
                        QStringLiteral("pane %1 showed no prompt in %2 ms: its shell has no shell integration, or "
                                       "has not started. %3")
                            .arg(QString::number(wait->pane), QString::number(wait->timeoutMs), m_words.noPrompt));
    }
    // The end of the wait came first.
    if (!wait->ready)
        return;
    switch (wait->until) {
    case Wait::Until::Text:
        // The text that came within the last event interval: one more check
        // reads it, and ends the wait.
        wait->lastCheck = true;
        return check(serial);
    case Wait::Until::Quiet:
    case Wait::Until::Closed:
        return finishWithText(serial, QStringLiteral("running"), {});
    case Wait::Until::Finished:
        break;
    }
    wait->ready = false;
    finishAtTimeout(serial);
}

void PaneRuns::finishAtTimeout(quint64 serial)
{
    const Wait& wait = *waitOf(serial);
    const qint64 pane = wait.pane;
    m_link.call(
        QStringLiteral("listPanes"), {}, [this, serial, pane](const QJsonValue& panes, const DaemonLink::Error* error) {
            if (!waitOf(serial))
                return;
            if (error)
                return failWait(serial, m_words.failure(*error));
            const QString shell = paneIn(panes, quint32(pane)).value(L("shell")).toString();
            m_link.call(QStringLiteral("shellRegions"), {pane, waitOf(serial)->after},
                        [this, serial, shell](const QJsonValue& value, const DaemonLink::Error* error) {
                            Wait* wait = waitOf(serial);
                            if (!wait)
                                return;
                            if (tryAgain(error)) {
                                return retryLater(serial, [this, serial] {
                                    finishAtTimeout(serial);
                                });
                            }
                            if (error)
                                return failWait(serial, m_words.failure(*error));
                            const QJsonArray regions = value.toArray();
                            // The end that came within the last event interval.
                            if (commandEnded(regions, wait->severalLines))
                                return finishWithText(serial, QStringLiteral("finished"), regions);
                            // A shell that holds the command unstarted at its
                            // prompt never ends it: no run waits for it.
                            if (wait->started.isValid() && shell == L("prompt") && !hasOutput(regions)) {
                                m_runs.remove(wait->pane);
                                return finishWithText(
                                    serial, QStringLiteral("running"), regions,
                                    QStringLiteral("The shell shows its prompt, and has not started the command: it "
                                                   "can hold an incomplete command (fish keeps one in its editor). %1")
                                        .arg(m_words.dropHeld));
                            }
                            finishWithText(serial, QStringLiteral("running"), regions);
                        });
        });
}

void PaneRuns::progress(quint64 serial)
{
    Wait* wait = waitOf(serial);
    if (!wait || !wait->ready || !wait->request->wantsProgress())
        return;
    if (wait->progressBusy) {
        wait->progressAgain = true;
        return;
    }
    wait->progressBusy = true;
    wait->progressAgain = false;
    const auto done = [this, serial] {
        if (Wait* wait = waitOf(serial)) {
            wait->progressBusy = false;
            if (wait->progressAgain)
                progress(serial);
        }
    };
    m_link.call(QStringLiteral("mark"), {qint64(wait->pane)},
                [this, serial, done](const QJsonValue& value, const DaemonLink::Error* error) {
                    Wait* wait = waitOf(serial);
                    if (!wait)
                        return;
                    // No mark: the count stays where it is.
                    if (error)
                        return done();
                    const QJsonObject mark = value.toObject();
                    const qint64 row = mark.value(L("row")).toInteger();
                    const qint64 epoch = epochOf(mark);
                    // The rows since the mark: a count that only grows. A reflow
                    // or a clear numbers the rows again, and the count goes on
                    // from them.
                    if (epoch != wait->progressEpoch) {
                        wait->progressEpoch = epoch;
                        wait->progressBase = row - qint64(wait->progressSent);
                    }
                    const double lines = double(row - wait->progressBase);
                    if (lines <= wait->progressSent)
                        return done();
                    m_link.call(QStringLiteral("getText"),
                                textArgs(wait->pane, QStringLiteral("since"),
                                         {{L("mark"), rowMark(std::max(wait->progressBase, row - 1), epoch)}}),
                                [this, serial, lines, done](const QJsonValue& value, const DaemonLink::Error* error) {
                                    Wait* wait = waitOf(serial);
                                    if (!wait)
                                        return;
                                    if (!error && lines > wait->progressSent) {
                                        // The newest line that has text, never the
                                        // whole output.
                                        QString newest;
                                        const QStringList rows =
                                            value.toObject().value(L("text")).toString().split(QLatin1Char('\n'));
                                        for (auto it = rows.crbegin(); it != rows.crend() && newest.isEmpty(); ++it)
                                            newest = it->trimmed();
                                        if (newest.size() > ProgressMessageLength) {
                                            newest.truncate(ProgressMessageLength);
                                            if (newest.back().isHighSurrogate())
                                                newest.chop(1);
                                        }
                                        wait->request->progress(lines, std::nullopt, newest);
                                        wait->progressSent = lines;
                                    }
                                    done();
                                });
                });
}

void PaneRuns::run(std::unique_ptr<PaneRequest> request)
{
    const quint32 pane = paneOf(*request);
    const QString command = request->arguments().value(L("command")).toString();
    const int timeoutMs =
        std::min(request->arguments().value(L("timeout_ms")).toInt(m_limits.waitTimeout), m_limits.maxWaitTimeout);
    if (m_held.contains(pane)) {
        request->fail(held(pane));
        return;
    }
    if (!DaemonLink::fits(QStringLiteral("paste"), {qint64(pane), command, runPaste()})) {
        request->fail(QStringLiteral("the command is longer than one paste can carry (1 MiB with its JSON escapes)"));
        return;
    }
    const int tailLines = request->arguments().value(L("tail_lines")).toInt(m_limits.tailLines);
    const quint64 serial = addWait(std::move(request), pane, true);
    if (!serial)
        return;
    // The output of the run is what the next wait reads, not the text since
    // a send before it.
    m_sendMarks.remove(pane);
    Wait& wait = *waitOf(serial);
    wait.run = true;
    wait.tailLines = tailLines;
    wait.command = command;
    wait.timeoutMs = timeoutMs;
    wait.deadline = QDeadlineTimer(timeoutMs);
    wait.severalLines = command.contains(QLatin1Char('\n')) || command.contains(QLatin1Char('\r'));
    prepareRun(serial);
}

void PaneRuns::prepareRun(quint64 serial)
{
    m_link.call(
        QStringLiteral("listPanes"), {}, [this, serial](const QJsonValue& panes, const DaemonLink::Error* error) {
            Wait* wait = waitOf(serial);
            if (!wait)
                return;
            if (error)
                return failWait(serial, m_words.failure(*error));
            const quint32 pane = wait->pane;
            const QJsonObject facts = paneIn(panes, pane);
            if (facts.isEmpty())
                return failWait(serial, noPane(pane));
            const QString shell = facts.value(L("shell")).toString();
            m_shells.insert(pane, shell);
            if (shell != L("none"))
                m_starting.remove(pane);
            else if (m_starting.contains(pane))
                return awaitPrompt(serial);
            const QString id = QString::number(pane);
            if (shell == L("none"))
                return failWait(serial,
                                QStringLiteral("pane %1 has no shell integration: %2").arg(id, m_words.sendAndWait));
            if (shell == L("running"))
                return failWait(serial, QStringLiteral("pane %1 runs a command: %2").arg(id, m_words.runningCommand));
            if (shell == L("continuation"))
                return failWait(
                    serial, QStringLiteral("pane %1 waits for the rest of a command: %2").arg(id, m_words.interrupt));
            if (facts.value(L("input")).toBool())
                return failWait(
                    serial,
                    QStringLiteral("pane %1 has text at its prompt that someone typed: %2").arg(id, m_words.typedText));
            startRun(serial);
        });
}

void PaneRuns::startRun(quint64 serial)
{
    const Wait& wait = *waitOf(serial);
    // Nothing went yet: a pane that decodes an image gets the calls again
    // a moment later, within the time of the call.
    const auto busy = [this, serial](const DaemonLink::Error& error) {
        if (waitOf(serial)->deadline.hasExpired())
            return failWait(serial, m_words.failure(error));
        retryLater(serial, [this, serial] {
            startRun(serial);
        });
    };
    m_link.call(QStringLiteral("mark"), {qint64(wait.pane)},
                [this, serial, busy](const QJsonValue& mark, const DaemonLink::Error* error) {
                    Wait* wait = waitOf(serial);
                    if (!wait)
                        return;
                    if (tryAgain(error))
                        return busy(*error);
                    if (error)
                        return failWait(serial, m_words.failure(*error));
                    wait->mark = mark.toObject();
                    wait->progressBase = wait->mark.value(L("row")).toInteger();
                    wait->progressEpoch = epochOf(wait->mark);
                    m_link.call(QStringLiteral("shellRegions"), {qint64(wait->pane)},
                                [this, serial, busy](const QJsonValue& regions, const DaemonLink::Error* error) {
                                    Wait* wait = waitOf(serial);
                                    if (!wait)
                                        return;
                                    if (tryAgain(error))
                                        return busy(*error);
                                    if (error)
                                        return failWait(serial, m_words.failure(*error));
                                    wait->after = newestRegion(regions.toArray());
                                    pasteCommand(serial);
                                });
                });
}

void PaneRuns::pasteCommand(quint64 serial)
{
    const Wait& wait = *waitOf(serial);
    const qint64 pane = wait.pane;
    // A first prompt that came near the end of the time: a command that
    // went now would run past the call, which tells its caller nothing.
    if (wait.deadline.hasExpired()) {
        return failWait(serial,
                        QStringLiteral("pane %1 was not ready for the command in %2 ms, and the command did "
                                       "not go: %3")
                            .arg(QString::number(pane), QString::number(wait.timeoutMs), m_words.runAgain));
    }
    // A paste, as a user's: bracketed when the shell asked for it, so that a
    // line end in it runs nothing before Enter. The daemon writes it only at
    // an empty prompt, and Enter with it: no other input comes between the
    // check and the command, and an end of this server or a cancel leaves no
    // command typed without its Enter.
    m_link.call(
        QStringLiteral("paste"), {pane, wait.command, runPaste()},
        [this, serial, pane](const QJsonValue&, const DaemonLink::Error* error) {
            Wait* wait = waitOf(serial);
            if (!wait)
                return;
            if (tryAgain(error) && !wait->deadline.hasExpired()) {
                return retryLater(serial, [this, serial] {
                    pasteCommand(serial);
                });
            }
            if (hasCode(error, AfterglowMux::Error::NotAtPrompt)) {
                return failWait(
                    serial,
                    QStringLiteral("the command did not go: %1. %2.").arg(error->message, m_words.paneState.arg(pane)));
            }
            if (error)
                return failWait(serial, m_words.failure(*error));
            wait->started.start();
            m_runs.insert(quint32(pane), RunRecord{wait->mark, wait->after, wait->started, wait->severalLines});
            startWaiting(serial);
        });
}

void PaneRuns::wait(std::unique_ptr<PaneRequest> request)
{
    const QJsonObject args = request->arguments();
    const quint32 pane = paneOf(*request);
    const QString until = args.value(L("until")).toString();
    const int timeoutMs = std::min(args.value(L("timeout_ms")).toInt(m_limits.waitTimeout), m_limits.maxWaitTimeout);
    const int tailLines = args.value(L("tail_lines")).toInt(m_limits.tailLines);
    const bool finished = until == L("finished");
    QRegularExpression pattern;
    if (until == L("text")) {
        if (!args.contains(L("pattern"))) {
            request->fail(m_words.needsPattern);
            return;
        }
        pattern = QRegularExpression(args.value(L("pattern")).toString(), QRegularExpression::MultilineOption);
        if (!pattern.isValid()) {
            request->fail(QStringLiteral("pattern is not a valid regular expression: %1 at offset %2")
                              .arg(pattern.errorString())
                              .arg(pattern.patternErrorOffset()));
            return;
        }
        pattern.optimize();
    }
    if (until == L("quiet") && !args.contains(L("quiet_ms"))) {
        request->fail(m_words.needsQuietMs);
        return;
    }
    if (finished && m_held.contains(pane)) {
        request->fail(held(pane));
        return;
    }
    const quint64 serial = addWait(std::move(request), pane, finished);
    if (!serial)
        return;
    Wait& wait = *waitOf(serial);
    wait.tailLines = tailLines;
    wait.timeoutMs = timeoutMs;
    wait.deadline = QDeadlineTimer(timeoutMs);
    wait.until = finished     ? Wait::Until::Finished
        : until == L("text")  ? Wait::Until::Text
        : until == L("quiet") ? Wait::Until::Quiet
                              : Wait::Until::Closed;
    wait.pattern = pattern;
    if (wait.until == Wait::Until::Quiet) {
        // The events of a pane come at most Control.eventInterval apart, and
        // as late: a shorter quiet time would end between two of them.
        wait.quietMs = std::max(args.value(L("quiet_ms")).toInt(), 2 * m_limits.eventInterval);
        connect(&wait.quiet, &QTimer::timeout, this, [this, serial] {
            finishWithText(serial, QStringLiteral("quiet"), {});
        });
    }
    // The mark first, before this function returns: the next request of the
    // client (a send that makes the text to wait for) goes after it. A mark
    // that the daemon answers with TRY_AGAIN goes again later, after that
    // request: then the text that the request made can come before the mark,
    // and not count.
    markWait(serial);
    startWait(serial);
}

void PaneRuns::markWait(quint64 serial)
{
    Wait& wait = *waitOf(serial);
    wait.markPending = true;
    m_link.call(QStringLiteral("mark"), {qint64(wait.pane)},
                [this, serial](const QJsonValue& value, const DaemonLink::Error* error) {
                    Wait* wait = waitOf(serial);
                    if (!wait)
                        return;
                    // A pane that decodes an image: again a moment later, within
                    // the time of the call.
                    if (tryAgain(error) && !wait->deadline.hasExpired()) {
                        return retryLater(serial, [this, serial] {
                            markWait(serial);
                        });
                    }
                    wait->markPending = false;
                    // A pane that does not exist: the pane list says what that means.
                    if (error) {
                        wait->markError = m_words.failure(*error);
                    } else {
                        wait->mark = value.toObject();
                        wait->searchMark = wait->mark;
                        wait->searchMark.insert(L("col"), 0);
                        // After a send, the line in progress still shows the
                        // prompt that the input answered: the text starts where
                        // the input went. A clear or a reflow since then: the
                        // line in progress is new.
                        if (wait->until == Wait::Until::Text) {
                            const QJsonObject sent = m_sendMarks.take(wait->pane);
                            if (!sent.isEmpty() && epochOf(sent) == epochOf(wait->mark)) {
                                wait->mark = sent;
                                wait->searchMark = sent;
                                wait->floor = sent;
                            }
                        }
                        wait->progressBase = wait->mark.value(L("row")).toInteger();
                        wait->progressEpoch = epochOf(wait->mark);
                    }
                    if (std::exchange(wait->startAfterMark, false))
                        startWait(serial);
                });
}

void PaneRuns::startWait(quint64 serial)
{
    m_link.call(
        QStringLiteral("listPanes"), {}, [this, serial](const QJsonValue& panes, const DaemonLink::Error* error) {
            Wait* wait = waitOf(serial);
            if (!wait)
                return;
            if (error)
                return failWait(serial, m_words.failure(*error));
            const quint32 pane = wait->pane;
            const bool seen = m_seen.contains(pane);
            const QJsonObject facts = paneIn(panes, pane);
            if (facts.isEmpty()) {
                // A pane that closed before the wait, and not one that never was.
                if (wait->until == Wait::Until::Closed && seen)
                    return finishClosed(serial, pane);
                return failWait(serial, noPane(pane));
            }
            if (wait->markPending) {
                wait->startAfterMark = true;
                return;
            }
            if (!wait->markError.isEmpty())
                return failWait(serial, wait->markError);
            const QString shell = facts.value(L("shell")).toString();
            m_shells.insert(pane, shell);
            if (shell != L("none"))
                m_starting.remove(pane);
            if (wait->until == Wait::Until::Quiet)
                wait->quiet.start(wait->quietMs);
            if (wait->until != Wait::Until::Finished)
                return startWaiting(serial);
            if (const auto record = m_runs.constFind(pane); record != m_runs.constEnd()) {
                wait->mark = record->mark;
                wait->after = record->after;
                wait->started = record->started;
                wait->severalLines = record->severalLines;
                wait->progressBase = wait->mark.value(L("row")).toInteger();
                wait->progressEpoch = epochOf(wait->mark);
                return startWaiting(serial);
            }
            if (shell == L("none") && m_starting.contains(pane))
                return awaitPrompt(serial);
            if (shell == L("none"))
                return failWait(serial,
                                QStringLiteral("pane %1 has no shell integration: %2")
                                    .arg(QString::number(pane), m_words.waitOutput));
            if (shell == L("continuation"))
                return failWait(serial,
                                QStringLiteral("pane %1 waits for the rest of a command: %2")
                                    .arg(QString::number(pane), m_words.interrupt));
            if (shell == L("prompt")) {
                // No run of this front end: the last command, which ended.
                const QJsonValue lastExit = facts.value(L("last_exit"));
                m_link.call(
                    QStringLiteral("getText"), textArgs(pane, QStringLiteral("last-output")),
                    [this, serial, lastExit](const QJsonValue& value, const DaemonLink::Error* error) {
                        Wait* wait = waitOf(serial);
                        if (!wait)
                            return;
                        if (tryAgain(error)) {
                            return retryLater(serial, [this, serial] {
                                startWait(serial);
                            });
                        }
                        if (error)
                            return failWait(serial, m_words.failure(*error));
                        const QJsonObject answer = value.toObject();
                        const Output output = shape(answer.value(L("text")).toString(), wait->tailLines,
                                                    m_limits.outputLimit, answer.value(L("truncated")).toBool());
                        QJsonObject result{{L("status"), L("finished")},
                                           {L("matched"), true},
                                           {L("match"), QJsonValue()},
                                           {L("exit_code"), lastExit.isDouble() ? lastExit : QJsonValue()},
                                           {L("exit_codes"), lastExit.isDouble() ? QJsonArray{lastExit} : QJsonArray()},
                                           {L("duration_ms"), QJsonValue()},
                                           {L("output"), output.text},
                                           {L("truncated"), output.truncated},
                                           {L("notes"), QJsonArray()}};
                        finishWait(
                            serial, result,
                            {QStringLiteral("Pane %1 is at its prompt: this is its last command.").arg(wait->pane)});
                    });
                return;
            }
            // A command that runs and was not this front end's: from now on.
            m_link.call(QStringLiteral("shellRegions"), {qint64(pane)},
                        [this, serial](const QJsonValue& regions, const DaemonLink::Error* error) {
                            Wait* wait = waitOf(serial);
                            if (!wait)
                                return;
                            if (tryAgain(error)) {
                                return retryLater(serial, [this, serial] {
                                    startWait(serial);
                                });
                            }
                            if (error)
                                return failWait(serial, m_words.failure(*error));
                            // Its output is the newest output region, also when
                            // it ended since the pane list: its exit code counts.
                            wait->after = runningSince(regions.toArray());
                            startWaiting(serial);
                        });
        });
}

void PaneRuns::send(std::unique_ptr<PaneRequest> request)
{
    const QJsonObject args = request->arguments();
    const qint64 pane = paneOf(*request);
    // In the order of the description: text, paste, keys, signal.
    SendCalls calls;
    if (args.contains(L("text")))
        calls.emplaceBack(QStringLiteral("sendText"), QJsonArray{pane, args.value(L("text"))});
    if (args.contains(L("paste")))
        calls.emplaceBack(QStringLiteral("paste"), QJsonArray{pane, args.value(L("paste"))});
    if (args.contains(L("keys")))
        calls.emplaceBack(QStringLiteral("sendKeys"), QJsonArray{pane, args.value(L("keys"))});
    if (args.contains(L("signal")))
        calls.emplaceBack(QStringLiteral("signalProgram"), QJsonArray{pane, args.value(L("signal"))});
    if (calls.isEmpty()) {
        request->fail(m_words.needsInput);
        return;
    }
    for (const auto& [fn, callArgs] : std::as_const(calls)) {
        if (!DaemonLink::fits(fn, callArgs)) {
            request->fail(
                QStringLiteral("the %1 is longer than one call can carry (1 MiB with its JSON escapes): send it "
                               "in parts")
                    .arg(sendPart(fn)));
            return;
        }
    }
    markSend(RequestPtr(std::move(request)), std::make_shared<const SendCalls>(std::move(calls)));
}

void PaneRuns::markSend(const RequestPtr& request, const std::shared_ptr<const SendCalls>& calls)
{
    const qint64 pane = request->arguments().value(L("pane")).toInteger();
    // Where the cursor is before the input goes: the next wait until text of
    // the pane reads from there, not the prompt that the input answers.
    m_link.call(QStringLiteral("mark"), {pane},
                [this, request, calls, pane](const QJsonValue& value, const DaemonLink::Error* error) {
                    if (!request->isValid())
                        return;
                    if (tryAgain(error)) {
                        QTimer::singleShot(retryMs(), this, [this, request, calls] {
                            if (request->isValid())
                                markSend(request, calls);
                        });
                        return;
                    }
                    // A pane that does not exist: the first part of the send says so.
                    // A wait until text that began before the send reads what the
                    // input makes: the mark would let the next wait match it again.
                    const bool covered = std::any_of(m_waits.begin(), m_waits.end(), [pane](const auto& entry) {
                        const Wait& wait = *entry.second;
                        return wait.pane == quint32(pane) && wait.until == Wait::Until::Text && !wait.markPending;
                    });
                    if (!error && !covered)
                        m_sendMarks.insert(quint32(pane), value.toObject());
                    else
                        m_sendMarks.remove(quint32(pane));
                    sendFrom(request, calls, 0);
                });
}

void PaneRuns::sendFrom(const RequestPtr& request, const std::shared_ptr<const SendCalls>& calls, qsizetype index)
{
    // One after the other: each goes when the one before it went.
    if (index == calls->size()) {
        request->finishText(QStringLiteral("Sent to pane %1.").arg(request->arguments().value(L("pane")).toInteger()));
        return;
    }
    m_link.call(calls->at(index).first, calls->at(index).second,
                [this, request, calls, index](const QJsonValue&, const DaemonLink::Error* error) {
                    // A pane that decodes an image took nothing of this part: it goes
                    // again a moment later, and what went before it never twice.
                    if (tryAgain(error)) {
                        QTimer::singleShot(retryMs(), this, [this, request, calls, index] {
                            if (request->isValid())
                                sendFrom(request, calls, index);
                        });
                        return;
                    }
                    if (!error)
                        return sendFrom(request, calls, index + 1);
                    if (index == 0) {
                        request->fail(m_words.failure(*error));
                        return;
                    }
                    QStringList went;
                    for (qsizetype i = 0; i < index; ++i)
                        went << sendPart(calls->at(i).first);
                    request->fail(
                        QStringLiteral("pane %1 took the %2, but not the %3: %4. Send only what did not go.")
                            .arg(request->arguments().value(L("pane")).toInteger())
                            .arg(went.join(QStringLiteral(" and ")), sendPart(calls->at(index).first), error->message));
                });
}

void PaneRuns::onEvent(const QString& name, const QJsonArray& args)
{
    const quint32 pane = quint32(args.at(0).toInteger());
    m_seen.insert(pane);
    const bool shellChanged = name == L("shellChanged");
    const bool closed = name == L("paneClosed");
    const bool textChanged = name == L("textChanged");
    if (shellChanged) {
        m_shells.insert(pane, args.at(1).toString());
        m_starting.remove(pane);
    }
    if (closed) {
        m_runs.remove(pane);
        m_sendMarks.remove(pane);
        m_shells.remove(pane);
        m_starting.remove(pane);
    }
    // The waits may end while this goes through them.
    std::vector<quint64> serials;
    for (const auto& [serial, wait] : m_waits) {
        if (wait->pane == pane)
            serials.push_back(serial);
    }
    for (const quint64 serial : serials) {
        Wait* wait = waitOf(serial);
        if (!wait)
            continue;
        if (closed) {
            // Also before the wait's own pane list came: the pane was there.
            if (wait->until == Wait::Until::Closed)
                finishClosed(serial, pane);
            else
                failWait(serial, QStringLiteral("pane %1 closed: its program ended, or the session ended").arg(pane));
            continue;
        }
        if (shellChanged && wait->awaitingPrompt) {
            // The first prompt, or what the shell did instead: the call goes on.
            wait->awaitingPrompt = false;
            wait->timeout.stop();
            if (wait->run)
                prepareRun(serial);
            else
                startWait(serial);
            continue;
        }
        if (textChanged) {
            if (wait->until == Wait::Until::Quiet && wait->ready && wait->quiet.isActive())
                wait->quiet.start();
            progress(serial);
        }
        // A new prompt, an incomplete command, or new text to match.
        if ((shellChanged && wait->until == Wait::Until::Finished)
            || (textChanged && wait->until != Wait::Until::Quiet)) {
            check(serial);
        }
    }
}

void PaneRuns::failAll(const QString& text)
{
    std::vector<quint64> serials;
    for (const auto& [serial, wait] : m_waits)
        serials.push_back(serial);
    for (const quint64 serial : serials)
        failWait(serial, text);
    m_runs.clear();
    m_starting.clear();
}

} // namespace Afterglow
