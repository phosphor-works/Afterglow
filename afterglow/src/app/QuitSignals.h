// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>

class QSocketNotifier;

namespace Afterglow {

/// Quits the event loop on SIGTERM, SIGINT or SIGHUP, so that windows, panes
/// and the control socket close as they do after the last window. Without it
/// the default action ends the process with no destructors, and the control
/// socket file stays. A second signal takes the earlier action at once, for a
/// shutdown that hangs. A signal that the process inherited as ignored stays
/// ignored (nohup). One instance at a time.
class QuitSignals : public QObject
{
    Q_OBJECT

public:
    explicit QuitSignals(QObject* parent = nullptr);
    /// Puts back the actions that the signals had before.
    ~QuitSignals() override;

    /// The signal that quit the event loop, or 0. The process should end
    /// with it after its shutdown, so that its parent sees why it ended.
    int signal() const { return m_signal; }

private:
    void deliver();

    QSocketNotifier* m_notifier = nullptr;
    int m_signal = 0;
};

} // namespace Afterglow
