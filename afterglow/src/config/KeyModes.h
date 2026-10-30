// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Keybinds.h"

namespace Afterglow {
/// Immutable bindings; each window owns an independent State.
class KeyModes
{
public:
    struct State {
        QList<int> prefix;
        QString table;
        qint64 deadline = 0;
        bool active() const { return !prefix.isEmpty() || !table.isEmpty(); }
        QString label() const;
    };
    struct Result { bool consumed = false; QString target; };
    static KeyModes fromConfig(const QVariantMap& bindings, const QVariantMap& tables,
        const QString& leader, int sequenceTimeout, int tableTimeout, const Keybinds& singles, QStringList* errors);
    Result press(const QKeyEvent* event, State& state, qint64 now) const;
    static void expire(State& state, qint64 now);
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
