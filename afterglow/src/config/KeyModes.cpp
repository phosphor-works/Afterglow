// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "KeyModes.h"
#include <QKeyEvent>
#include <QKeySequence>
#include <algorithm>

namespace Afterglow {
namespace {
/// Keys in @p text as QKeySequence::fromString splits them, without its limit
/// of four, so a longer sequence is reported instead of cut short. A comma is
/// a key when it ends the text or follows "+" ("Ctrl+,").
int keyCount(QString text)
{
    int n = 0;
    while (!text.isEmpty()) {
        qsizetype p = text.indexOf(QLatin1Char(','));
        if (p != -1) {
            if (p == text.size() - 1) {
                p = -1;
            } else {
                if (text.at(p + 1) == QLatin1Char(',')) ++p;
                if (p + 1 < text.size() && text.at(p + 1) == QLatin1Char(' ')) ++p;
            }
        }
        text = p == -1 ? QString() : text.mid(p + 1);
        ++n;
    }
    return n;
}

/// Presses that only change modifiers or the level: they neither advance nor cancel a mode.
bool modifierOnly(int key)
{
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_Hyper_L:
    case Qt::Key_Hyper_R:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
        return true;
    default:
        return false;
    }
}

void arm(KeyModes::State& state)
{
    if (!state.active()) {
        state = {};
    } else if (state.prefix.isEmpty()) {
        state.deadline = state.tableDeadline;
    } else {
        state.deadline = state.table.isEmpty() ? state.prefixDeadline : std::min(state.prefixDeadline, state.tableDeadline);
    }
}
} // namespace

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
    result.m_sequenceTimeout = sequenceTimeout;
    result.m_tableTimeout = tableTimeout;
    auto error = [&](const QString& text) { if (errors) errors->append(text); };
    const QLatin1String tablePrefix(Keybinds::TablePrefix);
    const QLatin1String textPrefix(Keybinds::TextPrefix);
    QStringList actions;
    for (Action a : Keybinds::allActions()) actions << Keybinds::token(a);
    auto add = [&](const QString& name, const QVariantMap& map) {
        // Keybinds::fromMap reads the root map too and reports its unknown
        // actions, bad text escapes and unparsable first keys; say them once.
        const bool root = name.isEmpty();
        auto& out = result.m_tables[name];
        for (auto it = map.begin(); it != map.end(); ++it) {
            const QString target = it.key();
            const bool tableAction = target.startsWith(tablePrefix);
            const bool pop = target == QLatin1String("pop_key_table");
            if (!actions.contains(target) && !target.startsWith(QLatin1String(Keybinds::ScriptPrefix))
                && !target.startsWith(textPrefix) && !tableAction && !pop) {
                if (!root) error(QStringLiteral("unknown key-table action %1").arg(target));
                continue;
            }
            if (root && pop) {
                // Outside a table it would only swallow its key.
                error(QStringLiteral("pop_key_table works only inside a key table"));
                continue;
            }
            if (tableAction && !tables.contains(target.mid(tablePrefix.size()))) {
                error(QStringLiteral("unknown key table %1").arg(target.mid(tablePrefix.size())));
                continue;
            }
            if (target.startsWith(textPrefix) && Keybinds::unescape(target.mid(textPrefix.size())).isEmpty()) {
                if (!root) error(QStringLiteral("invalid text binding %1").arg(target));
                continue;
            }
            QStringList sequences;
            if (it.value().typeId() == QMetaType::QString) {
                sequences << it.value().toString();
            } else if (it.value().typeId() == QMetaType::QVariantList || it.value().typeId() == QMetaType::QStringList) {
                for (const QVariant& v : it.value().toList()) {
                    if (v.typeId() == QMetaType::QString) sequences << v.toString();
                    else if (!root || tableAction) error(QStringLiteral("binding for %1 must be a key string or a list of them").arg(target));
                }
            } else if (!root || tableAction) {
                error(QStringLiteral("binding for %1 must be a key string or a list of them").arg(target));
            }
            for (QString text : std::as_const(sequences)) {
                text = text.trimmed();
                if (text.isEmpty()) continue;
                const bool leaderUsed = Keybinds::usesLeader(text);
                if (leaderUsed) {
                    if (leader.isEmpty()) { error(QStringLiteral("Leader used without Keybinds.leader")); continue; }
                    // Only a leading Leader token is substituted.
                    text.replace(0, 6, leader.trimmed());
                }
                if (keyCount(text) > MaxSequenceKeys) {
                    error(QStringLiteral("%1 has more than %2 keys").arg(text).arg(MaxSequenceKeys));
                    continue;
                }
                const QKeySequence sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
                Binding binding{{}, target};
                bool valid = !sequence.isEmpty();
                for (int i = 0; i < sequence.count(); ++i) {
                    valid &= sequence[i].key() != Qt::Key_unknown && sequence[i].key() != 0;
                    // The same modifiers as Keybinds::combinations, so keypad keys match.
                    binding.keys << QKeyCombination(sequence[i].keyboardModifiers() & Keybinds::RelevantMods,
                                                    sequence[i].key()).toCombined();
                }
                if (!valid) {
                    const bool reported = root && !leaderUsed && (sequence.isEmpty() || sequence[0].key() == Qt::Key_unknown);
                    if (!reported) error(QStringLiteral("invalid key sequence %1").arg(text));
                    continue;
                }
                // Root single keys are Keybinds' own, except a bare Leader and table entries.
                if (root && binding.keys.size() == 1 && !tableAction && !leaderUsed) continue;
                if (root && singles.isBound(binding.keys.front())) {
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
    // A table outlives a prefix that timed out inside it.
    if (!state.prefix.isEmpty() && now >= state.prefixDeadline) state.prefix.clear();
    if (!state.table.isEmpty() && now >= state.tableDeadline) state = {};
    arm(state);
}

KeyModes::Result KeyModes::press(const QKeyEvent* event, State& state, qint64 now) const
{
    expire(state, now);
    if (modifierOnly(event->key())) return {state.active(), {}};
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
                    if (!state.table.isEmpty()) state.tableDeadline = now + m_tableTimeout;
                    if (binding.keys.size() > prefix.size()) {
                        state.prefix = prefix; state.prefixDeadline = now + m_sequenceTimeout;
                        arm(state);
                        return {true, {}};
                    }
                    state.prefix.clear();
                    if (binding.target.startsWith(QLatin1String(Keybinds::TablePrefix))) {
                        state.table = binding.target.mid(int(qstrlen(Keybinds::TablePrefix)));
                        state.tableDeadline = now + m_tableTimeout;
                        arm(state);
                        return {true, {}};
                    }
                    // Leaves every table, not one level.
                    if (binding.target == QLatin1String("pop_key_table")) { state = {}; return {true, {}}; }
                    arm(state);
                    return {true, binding.target};
                }
            }
        }
        if (!state.active()) break;
        // Checked after the table, so a table can bind Escape itself.
        if (event->key() == Qt::Key_Escape) { state = {}; return {true, {}}; }
        // Holding the key that entered a table must not type it.
        if (event->isAutoRepeat()) return {true, {}};
        state = {};
    }
    return {};
}

QStringList KeyModes::bindingsOf(const QString& target) const
{
    QStringList out;
    for (const auto& binding : m_tables.value(QString())) {
        if (binding.target != target) continue;
        int keys[MaxSequenceKeys] = {};
        for (qsizetype i = 0; i < binding.keys.size() && i < MaxSequenceKeys; ++i) keys[i] = binding.keys[i];
        out << QKeySequence(keys[0], keys[1], keys[2], keys[3]).toString(QKeySequence::NativeText);
    }
    return out;
}
}
