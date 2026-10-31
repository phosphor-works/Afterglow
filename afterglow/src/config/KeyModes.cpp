// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "KeyModes.h"
#include <QKeyEvent>
#include <QKeySequence>
#include <algorithm>

namespace Afterglow {
QString KeyModes::State::label() const
{
    QStringList keys;
    for (int key : prefix) keys << QKeySequence(key).toString(QKeySequence::NativeText);
    return table.isEmpty() ? keys.join(QStringLiteral(", "))
        : table + (keys.isEmpty() ? QString() : QStringLiteral(": ") + keys.join(QStringLiteral(", ")));
}

KeyModes KeyModes::fromConfig(const QVariantMap& bindings, const QVariantMap& tables,
    const QString& leader, int sequenceTimeout, int tableTimeout, const Keybinds& singles, QStringList* errors)
{
    KeyModes result;
    result.m_sequenceTimeout = std::clamp(sequenceTimeout, 100, 10000);
    result.m_tableTimeout = std::clamp(tableTimeout, 100, 60000);
    auto error = [&](const QString& text) { if (errors) errors->append(text); };
    QStringList actions;
    for (Action a : Keybinds::allActions()) actions << Keybinds::token(a);
    auto add = [&](const QString& name, const QVariantMap& map) {
        auto& out = result.m_tables[name];
        for (auto it = map.begin(); it != map.end(); ++it) {
            const QString target = it.key();
            const bool tableAction = target.startsWith(QStringLiteral("table:"));
            if (!actions.contains(target) && !target.startsWith(QStringLiteral("script:"))
                && !target.startsWith(QStringLiteral("text:")) && !tableAction && target != QLatin1String("pop_key_table")) {
                error(QStringLiteral("unknown key-table action %1").arg(target));
                continue;
            }
            if (tableAction && !tables.contains(target.mid(6))) {
                error(QStringLiteral("unknown key table %1").arg(target.mid(6)));
                continue;
            }
            if (target.startsWith(QStringLiteral("text:")) && Keybinds::unescape(target.mid(5)).isEmpty()) {
                error(QStringLiteral("invalid text binding %1").arg(target));
                continue;
            }
            const QStringList sequences = it.value().typeId() == QMetaType::QString
                ? QStringList{it.value().toString()} : it.value().toStringList();
            for (QString text : sequences) {
                if (text.trimmed().isEmpty()) continue;
                // Only a leading standalone Leader token is substituted.
                if (text == QLatin1String("Leader") || text.startsWith(QStringLiteral("Leader,"))) {
                    if (leader.isEmpty()) { error(QStringLiteral("Leader used without Keybinds.leader")); continue; }
                    text.replace(0, 6, leader);
                }
                const QKeySequence sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
                Binding binding{{}, target};
                bool valid = !sequence.isEmpty();
                for (int i = 0; i < sequence.count(); ++i) {
                    valid &= sequence[i].key() != Qt::Key_unknown && sequence[i].key() != 0;
                    binding.keys << sequence[i].toCombined();
                }
                if (!valid) { error(QStringLiteral("invalid key sequence %1").arg(text)); continue; }
                if (name.isEmpty() && binding.keys.size() == 1 && !tableAction && target != QLatin1String("pop_key_table")) continue;
                if (name.isEmpty() && singles.isBound(binding.keys.front())) {
                    error(QStringLiteral("sequence %1 conflicts with an existing single-key binding. Unbind it first").arg(text));
                    continue;
                }
                bool conflict = false;
                for (const auto& previous : out) {
                    const auto n = std::min(previous.keys.size(), binding.keys.size());
                    if (std::equal(previous.keys.begin(), previous.keys.begin() + n, binding.keys.begin())) conflict = true;
                }
                if (conflict) { error(QStringLiteral("ambiguous key sequence %1 in table %2").arg(text, name)); continue; }
                out << binding;
            }
        }
    };
    add({}, bindings);
    for (auto it = tables.begin(); it != tables.end(); ++it) {
        if (it.key().isEmpty() || it.value().typeId() != QMetaType::QVariantMap) {
            error(QStringLiteral("key tables require a nonempty name and an action map"));
        } else add(it.key(), it.value().toMap());
    }
    return result;
}
void KeyModes::expire(State& state, qint64 now)
{
    if (state.active() && now >= state.deadline) state = {};
}
KeyModes::Result KeyModes::press(const QKeyEvent* event, State& state, qint64 now) const
{
    expire(state, now);
    if (state.active() && event->key() == Qt::Key_Escape) { state = {}; return {true, {}}; }
    if (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Control
        || event->key() == Qt::Key_Alt || event->key() == Qt::Key_Meta) return {state.active(), {}};
    if (event->isAutoRepeat() && !state.prefix.isEmpty()) return {true, {}};
    const auto combinations = Keybinds::combinations(event);
    // On mismatch discard the prefix/table and reconsider only this key at root.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto entries = m_tables.constFind(state.table);
        if (entries != m_tables.constEnd()) {
            for (int key : combinations) {
                QList<int> prefix = state.prefix; prefix << key;
                for (const auto& binding : *entries) {
                    if (binding.keys.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), binding.keys.begin())) continue;
                    if (binding.keys.size() > prefix.size()) {
                        state.prefix = prefix; state.deadline = now + m_sequenceTimeout;
                        return {true, {}};
                    }
                    state.prefix.clear();
                    if (binding.target.startsWith(QStringLiteral("table:"))) {
                        state.table = binding.target.mid(6); state.deadline = now + m_tableTimeout;
                        return {true, {}};
                    }
                    if (binding.target == QLatin1String("pop_key_table")) { state = {}; return {true, {}}; }
                    state.deadline = now + m_tableTimeout;
                    return {true, binding.target};
                }
            }
        }
        if (!state.active()) break;
        state = {};
    }
    return {};
}
}
