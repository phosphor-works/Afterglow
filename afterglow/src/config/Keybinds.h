// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QStringList>
#include <QVariantMap>

#include <optional>

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
    UrlsOpen,
    UrlsCopy,
    UnicodeInput,
    SelectAll,
    SplitLeft,
    SplitUp,
    ClearScreen,
    ResetTerminal,
    RenamePane,
    RenameTab,
    CloseWindow,
    NotifyOnFinish,
    MoveTabLeft,
    MoveTabRight,
    GotoTab1,
    GotoTab2,
    GotoTab3,
    GotoTab4,
    GotoTab5,
    GotoTab6,
    GotoTab7,
    GotoTab8,
    GotoTab9,
    ToggleFullscreen,
    TogglePaneZoom,
    ResizeLeft,
    ResizeRight,
    ResizeUp,
    ResizeDown,
    CopyMode,
    ShowLastOutput,
    ShowHints,
    SwapLeft, SwapRight, SwapUp, SwapDown,
    MovePaneToTab, MovePaneToWindow, DetachTab,
};

/// Key combinations bound to actions. Config form: { "split_right": "Ctrl+Shift+E",
/// "font_bigger": ["Ctrl+=", "Ctrl++"], "copy": "" } where an empty string
/// unbinds the action. Script actions bind as "script:<script id>.<action id>".
class Keybinds
{
public:
    static constexpr const char* ScriptPrefix = "script:";
    /// "text:<bytes>" sends the bytes to the terminal, with C escapes:
    /// \e \r \n \t \\ \xHH \uHHHH. { "text:\\x1b\\r": "Shift+Return" }
    static constexpr const char* TextPrefix = "text:";

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
    /// Actions that create or destroy panes, tabs or windows.
    static bool isStructural(Action action);

    /// The action bound to a key press, or Action::None.
    Action match(const QKeyEvent* event) const;
    static QList<int> combinations(const QKeyEvent* event);
    bool isBound(int key) const { return m_byKey.contains(key) || m_scriptByKey.contains(key) || m_textByKey.contains(key); }
    /// The script action ("<script id>.<action id>") bound to a key press, or empty.
    QString matchScript(const QKeyEvent* event) const;
    /// The bytes a "text:" binding sends for a key press, or empty.
    QByteArray matchText(const QKeyEvent* event) const;
    /// A "text:" binding's escapes turned into bytes; empty if malformed.
    static QByteArray unescape(const QString& text);
    /// Human-readable bindings of @p action, for menus and the command palette.
    QStringList bindingsOf(Action action) const;
    QStringList bindingsOfScript(const QString& action) const;

    bool operator==(const Keybinds&) const = default;

private:
    static std::optional<int> parse(const QString& sequence, const QString& target, QStringList* errors);
    void bind(Action action, const QString& sequence, QStringList* errors);

    QHash<int, Action> m_byKey; // QKeyCombination::toCombined()
    QHash<int, QString> m_scriptByKey;
    QHash<int, QByteArray> m_textByKey;
};

} // namespace Afterglow
