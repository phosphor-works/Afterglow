// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Keybinds.h"

namespace Afterglow {
/// Immutable bindings; each window owns an independent State.
class KeyModes
{
public:
    /// QKeySequence holds no more keys than this.
    static constexpr int MaxSequenceKeys = 4;
    struct State {
        QList<int> prefix;
        QString table;
        qint64 prefixDeadline = 0; ///< when a pending prefix times out
        qint64 tableDeadline = 0; ///< when an idle table is left
        qint64 deadline = 0; ///< the earlier of the two that apply: when expire() next has work
        bool active() const { return !prefix.isEmpty() || !table.isEmpty(); }
        QString label() const;
    };
    struct Result { bool consumed = false; QString target; };
    /// Timeouts arrive already clamped by the schema.
    static KeyModes fromConfig(const QVariantMap& bindings, const QVariantMap& tables,
        const QString& leader, int sequenceTimeout, int tableTimeout, const Keybinds& singles, QStringList* errors);
    Result press(const QKeyEvent* event, State& state, qint64 now) const;
    /// Drops a prefix that timed out, and leaves a table that stayed idle too long.
    static void expire(State& state, qint64 now);
    /// The root sequences bound to @p target ("new_tab", "script:a.b"), for
    /// menus and the palette, in the order the config lists them.
    QStringList bindingsOf(const QString& target) const;
    bool operator==(const KeyModes&) const = default;
private:
    struct Binding {
        QList<int> keys;
        QString target;
        bool operator==(const Binding&) const = default;
    };
    QHash<QString, QList<Binding>> m_tables;
    int m_sequenceTimeout = 1000;
    int m_tableTimeout = 5000;
};
}
