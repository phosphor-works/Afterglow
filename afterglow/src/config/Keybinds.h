// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QStringList>
#include <QVariantMap>

class QKeyEvent;

namespace Afterglow {

enum class Action {
    None,
    SplitRight,
    SplitDown,
    ClosePane,
    NewWindow,
    NewTab,
    CloseTab,
    NextTab,
    PreviousTab,
    Copy,
    Paste,
    FocusLeft,
    FocusRight,
    FocusUp,
    FocusDown,
    EqualizeSplits,
    FontBigger,
    FontSmaller,
    FontReset,
    ScrollPageUp,
    ScrollPageDown,
    ScrollTop,
    ScrollBottom,
    JumpPromptUp,
    JumpPromptDown,
    Search,
    CommandPalette,
    ReloadConfig,
    OpenConfig,
};

/// Key combinations bound to actions. Config form: { "split_right": "Ctrl+Shift+E",
/// "font_bigger": ["Ctrl+=", "Ctrl++"], "copy": "" } where an empty string
/// unbinds the action.
class Keybinds
{
public:
    static Keybinds defaults();
    /// Default bindings in config form, for the schema default.
    static QVariantMap defaultMap();
    /// Builds bindings from config, falling back to the defaults for actions
    /// not mentioned. Unknown actions and unparsable keys are reported in @p errors.
    static Keybinds fromMap(const QVariantMap& map, QStringList* errors = nullptr);

    static QString token(Action action);
    /// Display name of an action, for the command palette.
    static QString title(Action action);
    static QList<Action> allActions();

    /// The action bound to a key press, or Action::None.
    Action match(const QKeyEvent* event) const;
    /// Human-readable bindings of @p action, for menus and the command palette.
    QStringList bindingsOf(Action action) const;

    bool operator==(const Keybinds&) const = default;

private:
    void bind(Action action, const QString& sequence, QStringList* errors);

    QHash<int, Action> m_byKey; // QKeyCombination::toCombined()
};

} // namespace Afterglow
