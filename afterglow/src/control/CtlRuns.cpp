// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/CtlRuns.h"

#include "config/Settings.h"
#include "control/PaneRuns.h"
#include "mcp/DaemonLink.h"

#include "MuxProtocol.h"

#include <AfterglowProto/IpcServer.h>

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>

#include <cstdio>
#include <memory>
#include <utility>

namespace Afterglow {

namespace {

/// The hints of the messages of PaneRuns, in the words of afterglow ctl.
PaneRunWords ctlWords(const QString& where)
{
    PaneRunWords words;
    words.where = where;
    // One call for each process: no run of the same afterglow ctl holds a pane.
    words.owner = QStringLiteral("this afterglow ctl");
    words.listPanes = QStringLiteral("afterglow ctl ls gives the panes");
    words.paneState = QStringLiteral("afterglow ctl ls says what pane %1 does");
    words.sendAndWait = QStringLiteral("use send-text and wait");
    words.noPrompt = QStringLiteral("Use send-text and wait.");
    words.runningCommand = QStringLiteral(
        "wait with afterglow ctl wait --until finished, or send ctrl+c with afterglow ctl send-key ctrl+c");
    words.interrupt = QStringLiteral("send ctrl+c with afterglow ctl send-key ctrl+c");
    words.typedText = QStringLiteral("ask the user, or clear it with afterglow ctl send-key ctrl+u");
    words.runAgain = QStringLiteral("run it again");
    words.dropIncomplete = QStringLiteral("Drop it with afterglow ctl send-key ctrl+c.");
    words.dropHeld = QStringLiteral(
        "afterglow ctl get-text shows the pane. Drop such a command with afterglow ctl send-key ctrl+c.");
    words.waitFinished = QStringLiteral("wait for it with afterglow ctl wait --until finished");
    words.waitOutput = QStringLiteral("wait with --until text or quiet");
    words.needsPattern = QStringLiteral("wait --until text needs --pattern");
    words.needsQuietMs = QStringLiteral("wait --until quiet needs --quiet-ms");
    words.needsInput = QStringLiteral("nothing to send");
    words.failure = [](const DaemonLink::Error& error) {
        if (error.code.isEmpty())
            return QStringLiteral("lost the connection: %1").arg(error.message);
        if (error.code == QLatin1String(AfterglowMux::Error::TryAgain))
            return QStringLiteral("%1. The pane is busy for a moment: try again.").arg(error.message);
        return error.message;
    };
    return words;
}

/// The end of one call of PaneRuns.
struct Outcome
{
    bool done = false;
    bool failed = false;
    QString error;
    QJsonObject structured;
    QStringList texts;
};

/// A call of PaneRuns for afterglow ctl: one process, one call, no progress
/// and no cancel. @p done runs once, at its end.
class CtlRequest : public PaneRequest
{
public:
    CtlRequest(QJsonObject arguments, std::shared_ptr<Outcome> outcome, std::function<void()> done)
        : m_arguments(std::move(arguments))
        , m_outcome(std::move(outcome))
        , m_done(std::move(done))
    {
    }

    QJsonObject arguments() const override
    {
        return m_arguments;
    }
    bool isValid() const override
    {
        return !m_outcome->done;
    }
    void finish(const QJsonObject& structured, const QStringList& texts) override
    {
        m_outcome->structured = structured;
        m_outcome->texts = texts;
        end();
    }
    void finishText(const QString& text) override
    {
        m_outcome->texts = {text};
        end();
    }
    void fail(const QString& text) override
    {
        m_outcome->failed = true;
        m_outcome->error = text;
        end();
    }
    bool wantsProgress() const override
    {
        return false;
    }
    void progress(double, std::optional<double>, const QString&) override
    {
    }
    void onCancelled(std::function<void()>) override
    {
    }

private:
    void end()
    {
        if (std::exchange(m_outcome->done, true))
            return;
        m_done();
    }

    QJsonObject m_arguments;
    std::shared_ptr<Outcome> m_outcome;
    std::function<void()> m_done;
};

void printError(const QString& text)
{
    std::fprintf(stderr, "afterglow ctl: %s\n", qPrintable(text));
}

/// Prints @p outcome as afterglow ctl run and wait do, and gives the exit status.
int report(const Outcome& outcome, bool json)
{
    if (outcome.failed) {
        printError(outcome.error);
        return CtlRunFailed;
    }
    const QJsonObject& result = outcome.structured;
    const QString status = result.value(QLatin1String("status")).toString();
    if (json) {
        const QByteArray line = QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n';
        std::fwrite(line.constData(), 1, std::size_t(line.size()), stdout);
    } else {
        QByteArray output = result.value(QLatin1String("output")).toString().toUtf8();
        if (!output.isEmpty() && !output.endsWith('\n'))
            output += '\n';
        std::fwrite(output.constData(), 1, std::size_t(output.size()), stdout);
        // At the timeout, what goes on and how to wait for it; else only the
        // notes, which say what the output alone does not.
        if (status == QLatin1String("running") && !outcome.texts.isEmpty()) {
            for (const QString& text : outcome.texts)
                printError(text);
        } else {
            for (const QJsonValue& note : result.value(QLatin1String("notes")).toArray())
                printError(note.toString());
        }
    }
    std::fflush(stdout);
    if (status == QLatin1String("running"))
        return CtlRunTimedOut;
    if (status == QLatin1String("finished")) {
        // A command that ran nothing (an empty line, a comment) has no code.
        const QJsonValue code = result.value(QLatin1String("exit_code"));
        if (code.isNull() || code.isUndefined())
            return 0;
        const qint64 value = code.toInteger();
        return value >= 0 && value <= 255 ? int(value) : 1;
    }
    return 0;
}

} // namespace

int runPaneCommand(int fd, const CtlRun& run, const Settings& settings)
{
    DaemonLink link(fd);
    PaneRuns runs(link, PaneRunLimits::of(settings), ctlWords(run.where));
    QObject::connect(&link, &DaemonLink::event, &runs, &PaneRuns::onEvent);
    QObject::connect(&link, &DaemonLink::closed, &runs, [&runs](const QString& reason) {
        runs.failAll(QStringLiteral("lost the connection: %1").arg(reason));
    });
    QObject::connect(&link, &DaemonLink::subscriptionEnded, &runs,
                     [&runs](const QString&, const QString& code, const QString& message) {
                         if (code == QLatin1String(AfterglowMux::Error::NoSuchSession))
                             runs.failAll(QStringLiteral("the session ended: %1").arg(message));
                         else
                             runs.failAll(message);
                     });

    QEventLoop loop;
    auto outcome = std::make_shared<Outcome>();
    const auto done = [&loop] {
        loop.quit();
    };
    const auto start = [&] {
        switch (run.kind) {
        case CtlRun::Kind::Run:
            runs.run(std::make_unique<CtlRequest>(run.arguments, outcome, done));
            break;
        case CtlRun::Kind::Wait:
            runs.wait(std::make_unique<CtlRequest>(run.arguments, outcome, done));
            break;
        case CtlRun::Kind::SendWait: {
            // The send takes the pane's mark before its input goes: the wait
            // reads from there, so the prompt that the input answers does not
            // match.
            auto sent = std::make_shared<Outcome>();
            runs.send(std::make_unique<CtlRequest>(run.arguments, sent, [&runs, &run, sent, outcome, done] {
                if (sent->failed) {
                    outcome->failed = true;
                    outcome->error = sent->error;
                    outcome->done = true;
                    done();
                    return;
                }
                runs.wait(std::make_unique<CtlRequest>(run.waitArguments, outcome, done));
            }));
            break;
        }
        }
    };
    // The events first: a run that starts before them would miss its end.
    const QStringList events = PaneRuns::events();
    auto waiting = std::make_shared<qsizetype>(events.size());
    for (const QString& event : events) {
        link.subscribe(event, [&, waiting](const QJsonValue&, const DaemonLink::Error* error) {
            if (outcome->done)
                return;
            if (error) {
                outcome->failed = true;
                outcome->error = error->code == QLatin1String(AfterglowProto::IpcErrorCode::NoSuchTarget)
                    ? QStringLiteral("Control.enabled is off, and the wait needs the events of the panes")
                    : ctlWords(run.where).failure(*error);
                outcome->done = true;
                done();
                return;
            }
            if (--*waiting == 0)
                start();
        });
    }
    if (!outcome->done)
        loop.exec();
    return report(*outcome, run.json);
}

} // namespace Afterglow
