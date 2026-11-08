// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ScriptHost.h"
#include <QHash>

namespace Afterglow {
/// Owns nonblocking work for each loaded script instance.
class ScriptTasks : public QObject
{
public:
    using Completion = std::function<void(ScriptHost::EffectResult)>;
    /// A task's call of a handler. @p task is the key of the timer or
    /// watcher, which differs for each task of the object's life. Every tick
    /// or change of one task gives the same key, so a call that waits can
    /// take the event of a later tick of its task. The receiver passes each
    /// call's key to claim() once, when the call runs or is dropped.
    using Event = std::function<void(const QString& handler, QVariantMap event, quint64 task)>;
    explicit ScriptTasks(QObject* parent = nullptr) : QObject(parent) {}
    ~ScriptTasks() override;
    ScriptHost::EffectResult apply(const QString& owner, const QVariantMap&, Completion, Event);
    void clear(const QString& owner);
    /// Stops the processes and file reads of @p owner whose request IDs
    /// start with @p prefix, and delivers no result: what an ended command
    /// run waited for.
    void cancel(const QString& owner, const QString& prefix);
    /// Whether the call of task @p key, which may have waited, can run:
    /// false once the task ended (cancelled, or its owner cleared). A
    /// one-shot timer stays active from its tick to this claim, so a cancel
    /// while its call waits stops the call, and ends here: its call can
    /// start a timer of the same name.
    bool claim(quint64 key);
private:
    struct Task;
    QHash<QString, QHash<QString, Task*>> m_tasks;
    QHash<quint64, Task*> m_keyed; ///< the timers and watchers, by key
    quint64 m_lastKey = 0;
    void finish(Task*, ScriptHost::EffectResult, bool deliver = true);
};
}
