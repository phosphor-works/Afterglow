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
    /// A task's call of a handler. @p repeating is not 0 for a task that
    /// calls again (a repeating timer, a watcher), and differs for each such
    /// task of the object's life: a call that waits can take the next one's
    /// news. A one-shot timer gives 0.
    using Event = std::function<void(const QString& handler, QVariantMap event, quint64 repeating)>;
    explicit ScriptTasks(QObject* parent = nullptr) : QObject(parent) {}
    ~ScriptTasks() override;
    ScriptHost::EffectResult apply(const QString& owner, const QVariantMap&, Completion, Event);
    void clear(const QString& owner);
    /// Stops the processes and file reads of @p owner whose request IDs
    /// start with @p prefix, and delivers no result: what an ended command
    /// run waited for.
    void cancel(const QString& owner, const QString& prefix);
private:
    struct Task;
    QHash<QString, QHash<QString, Task*>> m_tasks;
    quint64 m_lastRepeating = 0;
    void finish(Task*, ScriptHost::EffectResult, bool deliver = true);
};
}
