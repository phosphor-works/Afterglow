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
/// it: a terminal of its own, drawn in place of the host pane's grid or as
/// a popup over a part of it, that takes the host's input while it runs. The host pane owns it and goes on
/// running underneath. It ends when the program exits, or with "cancelled"
/// when it is cancelled or deleted first (its host closes, its owner
/// retires, the command run that opened it ends, the control client that
/// opened it disconnects, the user cancels it). Its program gets SIGHUP
/// when it is deleted.
class PaneOverlay : public QObject
{
    Q_OBJECT

public:
    using Reply = OverlayReply;

    /// What the window gives the program, as for a pane of its own.
    struct Launch
    {
        AfterglowVt::TerminalOptions terminal;
        AfterglowPty::SpawnOptions spawn; ///< the configured environment
        QString term;
        /// The host's grid, which the overlay takes whole or in part (a popup).
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

    /// The spec's title, which a popup's border shows (UTF-8 there).
    const QString& title() const { return m_title; }
    const std::string& borderTitle() const { return m_borderTitle; }
    /// A box over its host, which shows around it (OverlaySpec::popup).
    bool popup() const { return m_shape.popup(); }
    /// Where its program's grid is in the host's grid, and its border.
    const OverlaySpec::Placement& placement() const { return m_placement; }
    /// Places it anew in a host grid of @p cols by @p rows and resizes its
    /// program, which hears of a change.
    void place(int cols, int rows, int cellWidth, int cellHeight);

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
    QString m_title;
    std::string m_borderTitle;
    OverlaySpec m_shape; ///< the size, position and border only
    OverlaySpec::Placement m_placement;
    QString m_owner;
    QString m_request;
    Reply m_reply;
    bool m_answered = false;
};

} // namespace Afterglow
