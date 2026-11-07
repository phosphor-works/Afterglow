// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "scripting/OverlaySpec.h"

#include <AfterglowPty/Pty.h>
#include <AfterglowVt/Terminal.h>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <memory>

class QTemporaryFile;

namespace Afterglow {

class Pane;

/// A program run over a pane, with its result returned to whoever opened
/// it: a terminal of its own, drawn in place of the host pane's grid, that
/// takes the host's input while it runs. The host pane owns it and goes on
/// running underneath. It ends when the program exits, or with "cancelled"
/// when it is cancelled or deleted first (its host closes, its owner
/// retires, the command run that opened it ends, the control client that
/// opened it disconnects, the user cancels it). Its program gets SIGHUP
/// when it is deleted.
class PaneOverlay : public QObject
{
    Q_OBJECT

public:
    /// The answer: an error and its code (cancelled), or the result
    /// {exit_code, stdout, truncated}. Called once.
    using Reply = std::function<void(const QString& error, const QString& code, const QVariantMap& result)>;

    /// What the window gives the program, as for a pane of its own.
    struct Launch
    {
        AfterglowVt::TerminalOptions terminal;
        AfterglowPty::SpawnOptions spawn; ///< the configured environment
        QString term;
        int cols = 80;
        int rows = 24;
        int cellWidth = 1;
        int cellHeight = 1;
    };

    /// Starts @p spec over @p host and returns it, owned by @p host, or null
    /// with @p error set when the program cannot start.
    static PaneOverlay* start(Pane& host, const OverlaySpec& spec, const Launch& launch, QString* error);
    /// Answers cancelled when it has not answered yet.
    ~PaneOverlay() override;

    /// Whom the answer goes to, and the script's request ID, for closing
    /// the overlays of an owner or of a run.
    void setReply(const QString& owner, const QString& request, Reply reply);
    const QString& owner() const { return m_owner; }
    const QString& request() const { return m_request; }

    /// The overlay's own terminal.
    Pane* pane() const { return m_pane; }

    /// Ends it now, with "cancelled" and @p message.
    void cancel(const QString& message);

Q_SIGNALS:
    /// The program exited or the overlay was cancelled. It has left its
    /// host already: the window takes it off screen and deletes it.
    void ended();

private:
    PaneOverlay(Pane& host);
    /// The program exited: its answer goes out.
    void programExited();
    /// The text of the result file, at most one byte over the limit: none
    /// when it is no regular file.
    static QByteArray readResult(const QString& path);
    /// Answers once, and leaves the host.
    void finish(const QString& error, const QString& code, const QVariantMap& result);

    Pane* m_host;
    Pane* m_pane = nullptr;
    OverlaySpec::Capture m_capture = OverlaySpec::Capture::Stdout;
    std::unique_ptr<QTemporaryFile> m_resultFile;
    QString m_owner;
    QString m_request;
    Reply m_reply;
    bool m_answered = false;
};

} // namespace Afterglow
