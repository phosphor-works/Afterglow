// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>

class QSocketNotifier;

namespace Afterglow {

/// Quits the event loop on SIGTERM, SIGINT or SIGHUP, so that windows, panes
/// and the control socket close as they do after the last window. Without it
/// the default action ends the process with no destructors, and the control
/// socket file stays. A second signal takes the default action at once, for a
/// shutdown that hangs. One instance at a time.
class QuitSignals : public QObject
{
    Q_OBJECT

public:
    explicit QuitSignals(QObject* parent = nullptr);
    ~QuitSignals() override;

Q_SIGNALS:
    /// Before the event loop quits.
    void received(int signal);

private:
    void deliver();

    QSocketNotifier* m_notifier = nullptr;
};

} // namespace Afterglow
