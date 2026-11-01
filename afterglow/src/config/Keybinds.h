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
    RotatePanesClockwise, RotatePanesCounterclockwise, PaneOverview, UsePackageShaders,
    MovePaneToTab, MovePaneToWindow, DetachTab,
    SaveProject, SwitchProject,
    EndScriptKeyMode, ///< always ends a script's key mode, so a script cannot keep the keyboard
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
    /// "table:<name>" activates a key table (KeyModes).
    static constexpr const char* TablePrefix = "table:";
    /// The modifiers that take part in matching; others (keypad, group switch) are ignored.
    static constexpr Qt::KeyboardModifiers RelevantMods =
        Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;

    static Keybinds defaults();
    /// Default bindings in config form, for the schema default.
    static QVariantMap defaultMap();
    /// Builds bindings from config, falling back to the defaults for actions
    /// not mentioned. A key that the config binds explicitly leaves every
    /// default that used it; two explicit bindings of one key are an error,
    /// and the first (in key order) keeps it. Unknown actions and unparsable
    /// keys are reported in @p errors.
    static Keybinds fromMap(const QVariantMap& map, QStringList* errors = nullptr);
    /// Whether @p sequence starts with the Leader token ("Leader" or "Leader, X"),
    /// in any case. KeyModes replaces it with Keybinds.leader.
    static bool usesLeader(const QString& sequence);

    static QString token(Action action);
    /// Display name of an action, for the command palette.
    static QString title(Action action);
    static QList<Action> allActions();
    /// Actions that create or destroy panes, tabs or windows.
    static bool isStructural(Action action);

    /// The action bound to a key press, or Action::None. @p base is the
    /// key's character on the active layout, as for combinations().
    Action match(const QKeyEvent* event, char32_t base = 0) const;
    /// The combinations a key press can match, most specific first: the event
    /// itself, Tab for Backtab, the layout's unshifted key, and a shifted
    /// symbol without its Shift ("%" for Shift+5 on a US layout).
    /// @p base is the character the key types without modifiers on the
    /// active layout, or on the first Latin layout when that one is not
    /// ASCII; 0 when unknown. The caller reads it from the keymap
    /// (bindingBaseKey in KeyMapping.h): the config layer follows no
    /// keyboard state of its own.
    static QList<int> combinations(const QKeyEvent* event, char32_t base = 0);
    bool isBound(int key) const { return m_byKey.contains(key) || m_scriptByKey.contains(key) || m_textByKey.contains(key); }
    /// The script action ("<script id>.<action id>") bound to a key press, or empty.
    QString matchScript(const QKeyEvent* event, char32_t base = 0) const;
    /// The bytes a "text:" binding sends for a key press, or empty.
    QByteArray matchText(const QKeyEvent* event, char32_t base = 0) const;
    /// A "text:" binding's escapes turned into bytes; empty if malformed.
    static QByteArray unescape(const QString& text);
    /// Human-readable bindings of @p action, for menus and the command palette,
    /// in the order the config lists them.
    QStringList bindingsOf(Action action) const;
    QStringList bindingsOfScript(const QString& action) const;

    bool operator==(const Keybinds&) const = default;

private:
    static std::optional<int> parse(const QString& sequence, const QString& target, QStringList* errors);
    /// Takes @p combo away from whatever holds it.
    void unbind(int combo);

    QHash<int, Action> m_byKey; // QKeyCombination::toCombined()
    QHash<int, QString> m_scriptByKey;
    QHash<int, QByteArray> m_textByKey;
    QList<int> m_order; ///< every bound combination, in binding order
};

} // namespace Afterglow
