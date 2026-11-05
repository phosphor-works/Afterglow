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
    using Event = std::function<void(const QString&, QVariantMap)>;
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
    void finish(Task*, ScriptHost::EffectResult, bool deliver = true);
};
}
