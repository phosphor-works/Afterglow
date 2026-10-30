// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Keybinds.h"

#include "../XkbKeyboard.h"

#include <QKeyEvent>
#include <QKeySequence>

#include <optional>

namespace Afterglow {

bool Keybinds::isStructural(Action action)
{
    switch (action) {
    case Action::SplitRight:
    case Action::SplitDown:
    case Action::SplitLeft:
    case Action::SplitUp:
    case Action::ClosePane:
    case Action::NewWindow:
    case Action::ShowLastOutput:
    case Action::NewTab:
    case Action::CloseTab:
    case Action::CloseWindow:
        return true;
    default:
        return false;
    }
}

namespace {

struct ActionInfo
{
    Action action;
    const char* token;
    const char* title;
    std::initializer_list<const char*> defaults;
};

const ActionInfo Actions[] = {
    {Action::SplitRight, "split_right", "Split Right", {"Ctrl+Shift+E"}},
    {Action::SplitDown, "split_down", "Split Down", {"Ctrl+Shift+O"}},
    {Action::SplitLeft, "split_left", "Split Left", {}},
    {Action::SplitUp, "split_up", "Split Up", {}},
    {Action::RenamePane, "rename_pane", "Change Split Title", {}},
    {Action::ClosePane, "close_pane", "Close Pane", {"Ctrl+Shift+W"}},
    {Action::NewWindow, "new_window", "New Window", {"Ctrl+Shift+N"}},
    {Action::NewTab, "new_tab", "New Tab", {"Ctrl+Shift+T"}},
    {Action::CloseTab, "close_tab", "Close Tab", {"Ctrl+Shift+Q"}},
    {Action::RenameTab, "rename_tab", "Change Tab Title", {}},
    {Action::MoveTabLeft, "move_tab_left", "Move Tab Left", {"Ctrl+Shift+Alt+PgUp"}},
    {Action::MoveTabRight, "move_tab_right", "Move Tab Right", {"Ctrl+Shift+Alt+PgDown"}},
    {Action::GotoTab1, "goto_tab_1", "Go to Tab 1", {"Alt+1"}},
    {Action::GotoTab2, "goto_tab_2", "Go to Tab 2", {"Alt+2"}},
    {Action::GotoTab3, "goto_tab_3", "Go to Tab 3", {"Alt+3"}},
    {Action::GotoTab4, "goto_tab_4", "Go to Tab 4", {"Alt+4"}},
    {Action::GotoTab5, "goto_tab_5", "Go to Tab 5", {"Alt+5"}},
    {Action::GotoTab6, "goto_tab_6", "Go to Tab 6", {"Alt+6"}},
    {Action::GotoTab7, "goto_tab_7", "Go to Tab 7", {"Alt+7"}},
    {Action::GotoTab8, "goto_tab_8", "Go to Tab 8", {"Alt+8"}},
    {Action::GotoTab9, "goto_tab_9", "Go to the Last Tab", {"Alt+9"}},
    {Action::CloseWindow, "close_window", "Close Window", {}},
    {Action::ToggleFullscreen, "toggle_fullscreen", "Toggle Fullscreen", {"F11"}},
    {Action::NextTab, "next_tab", "Next Tab", {"Ctrl+Tab", "Ctrl+PgDown"}},
    {Action::PreviousTab, "previous_tab", "Previous Tab", {"Ctrl+Shift+Tab", "Ctrl+PgUp"}},
    {Action::ShowHints, "show_hints", "Select Text or File Reference", {"Ctrl+Shift+H"}},
    {Action::ShowLastOutput, "show_last_output", "View Last Command Output in Pager", {}},
    {Action::CopyMode, "copy_mode", "Keyboard Copy Mode", {"Ctrl+Shift+Space"}},
    {Action::Copy, "copy", "Copy", {"Ctrl+Shift+C"}},
    {Action::Paste, "paste", "Paste", {"Ctrl+Shift+V"}},
    {Action::SelectAll, "select_all", "Select All", {"Ctrl+Shift+A"}},
    {Action::ClearScreen, "clear_screen", "Clear Screen and Scrollback", {"Ctrl+Shift+K"}},
    {Action::ResetTerminal, "reset", "Reset Terminal", {}},
    {Action::NotifyOnFinish, "notify_on_finish", "Notify When the Next Command Finishes", {}},
    {Action::FocusLeft, "focus_left", "Focus Split Left", {"Ctrl+Shift+Left"}},
    {Action::FocusRight, "focus_right", "Focus Split Right", {"Ctrl+Shift+Right"}},
    {Action::FocusUp, "focus_up", "Focus Split Above", {"Ctrl+Shift+Up"}},
    {Action::FocusDown, "focus_down", "Focus Split Below", {"Ctrl+Shift+Down"}},
    {Action::EqualizeSplits, "equalize_splits", "Equalize Splits", {"Ctrl+Shift+="}},
    {Action::TogglePaneZoom, "toggle_pane_zoom", "Toggle Pane Zoom", {"Ctrl+Shift+Return"}},
    {Action::ResizeLeft, "resize_left", "Resize Split Left", {"Ctrl+Shift+Alt+Left"}},
    {Action::ResizeRight, "resize_right", "Resize Split Right", {"Ctrl+Shift+Alt+Right"}},
    {Action::ResizeUp, "resize_up", "Resize Split Up", {"Ctrl+Shift+Alt+Up"}},
    {Action::ResizeDown, "resize_down", "Resize Split Down", {"Ctrl+Shift+Alt+Down"}},
    {Action::FontBigger, "font_bigger", "Increase Font Size", {"Ctrl+=", "Ctrl++"}},
    {Action::FontSmaller, "font_smaller", "Decrease Font Size", {"Ctrl+-"}},
    {Action::FontReset, "font_reset", "Reset Font Size", {"Ctrl+0"}},
    {Action::ScrollPageUp, "scroll_page_up", "Scroll Page Up", {"Shift+PgUp"}},
    {Action::ScrollPageDown, "scroll_page_down", "Scroll Page Down", {"Shift+PgDown"}},
    {Action::ScrollTop, "scroll_top", "Scroll to Top", {"Shift+Home"}},
    {Action::ScrollBottom, "scroll_bottom", "Scroll to Bottom", {"Shift+End"}},
    {Action::JumpPromptUp, "jump_prompt_up", "Jump to Previous Prompt", {"Ctrl+Shift+Z"}},
    {Action::JumpPromptDown, "jump_prompt_down", "Jump to Next Prompt", {"Ctrl+Shift+X"}},
    {Action::Search, "search", "Search", {"Ctrl+Shift+F"}},
    {Action::CommandPalette, "command_palette", "Command Palette", {"Ctrl+Shift+P"}},
    {Action::ReloadConfig, "reload_config", "Reload Config", {"Ctrl+Shift+R"}},
    {Action::OpenConfig, "open_config", "Open Config File", {"Ctrl+Shift+,"}},
    {Action::UrlsOpen, "show_urls_open", "Open a URL on Screen", {"Ctrl+Shift+G"}},
    {Action::UrlsCopy, "show_urls_copy", "Copy a URL on Screen", {"Ctrl+Shift+Y"}},
    {Action::UnicodeInput, "unicode_input", "Insert Unicode Character", {"Ctrl+Shift+U"}},
};

constexpr Qt::KeyboardModifiers RelevantMods =
    Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;

} // namespace

QString Keybinds::token(Action action)
{
    for (const ActionInfo& a : Actions) {
        if (a.action == action) {
            return QLatin1String(a.token);
        }
    }
    return {};
}

QString Keybinds::title(Action action)
{
    for (const ActionInfo& a : Actions) {
        if (a.action == action) {
            return QString::fromUtf8(a.title);
        }
    }
    return {};
}

QList<Action> Keybinds::allActions()
{
    QList<Action> out;
    for (const ActionInfo& a : Actions) {
        out.append(a.action);
    }
    return out;
}

QVariantMap Keybinds::defaultMap()
{
    QVariantMap map;
    for (const ActionInfo& a : Actions) {
        if (a.defaults.size() == 1) {
            map.insert(QLatin1String(a.token), QString::fromLatin1(*a.defaults.begin()));
        } else {
            QStringList list;
            for (const char* k : a.defaults) {
                list.append(QString::fromLatin1(k));
            }
            map.insert(QLatin1String(a.token), list);
        }
    }
    return map;
}

Keybinds Keybinds::defaults()
{
    return fromMap(defaultMap());
}

std::optional<int> Keybinds::parse(const QString& sequence, const QString& target, QStringList* errors)
{
    if (sequence.trimmed().isEmpty() || sequence.startsWith(QStringLiteral("Leader,"))) {
        return std::nullopt;
    }
    const QKeySequence seq = QKeySequence::fromString(sequence, QKeySequence::PortableText);
    if (seq.isEmpty() || seq[0].key() == Qt::Key_unknown) {
        if (errors) {
            errors->append(QStringLiteral("cannot parse key \"%1\" for %2").arg(sequence, target));
        }
        return std::nullopt;
    }
    if (seq.count() > 1) return std::nullopt; // handled by KeyModes
    return QKeyCombination(seq[0].keyboardModifiers() & RelevantMods, seq[0].key()).toCombined();
}

void Keybinds::bind(Action action, const QString& sequence, QStringList* errors)
{
    if (const auto combo = parse(sequence, token(action), errors)) {
        m_byKey.insert(*combo, action);
    }
}

Keybinds Keybinds::fromMap(const QVariantMap& map, QStringList* errors)
{
    Keybinds kb;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        bool known = it.key().startsWith(QStringLiteral("table:")) || it.key() == QLatin1String("pop_key_table");
        for (const ActionInfo& a : Actions) {
            if (it.key() == QLatin1String(a.token)) {
                known = true;
            }
        }
        if (!known && it.key().startsWith(QLatin1String(ScriptPrefix))) {
            const QVariant v = it.value();
            const QStringList seqs = v.typeId() == QMetaType::QString ? QStringList{v.toString()} : v.toStringList();
            for (const QString& s : seqs) {
                if (const auto combo = parse(s, it.key(), errors)) {
                    kb.m_scriptByKey.insert(*combo, it.key().mid(int(qstrlen(ScriptPrefix))));
                }
            }
            known = true;
        }
        if (!known && it.key().startsWith(QLatin1String(TextPrefix))) {
            const QByteArray bytes = unescape(it.key().mid(int(qstrlen(TextPrefix))));
            if (bytes.isEmpty()) {
                if (errors) {
                    errors->append(QStringLiteral("\"%1\" sends nothing; check its escapes").arg(it.key()));
                }
            } else {
                const QVariant v = it.value();
                const QStringList seqs = v.typeId() == QMetaType::QString ? QStringList{v.toString()} : v.toStringList();
                for (const QString& s : seqs) {
                    if (const auto combo = parse(s, it.key(), errors)) {
                        kb.m_textByKey.insert(*combo, bytes);
                    }
                }
            }
            known = true;
        }
        if (!known && errors) {
            errors->append(QStringLiteral("unknown action \"%1\"").arg(it.key()));
        }
    }
    for (const ActionInfo& a : Actions) {
        const QString key = QLatin1String(a.token);
        if (!map.contains(key)) {
            for (const char* d : a.defaults) {
                kb.bind(a.action, QString::fromLatin1(d), errors);
            }
            continue;
        }
        const QVariant v = map.value(key);
        const QStringList seqs = v.typeId() == QMetaType::QString ? QStringList{v.toString()} : v.toStringList();
        for (const QString& s : seqs) {
            kb.bind(a.action, s, errors);
        }
    }
    return kb;
}

QList<int> Keybinds::combinations(const QKeyEvent* event)
{
    constexpr auto modsMask = Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
    const auto mods = event->modifiers() & modsMask;
    QList<int> result{QKeyCombination(mods, Qt::Key(event->key())).toCombined()};
    const char32_t base = XkbKeyboard::instance().unshiftedCodepoint(event->nativeScanCode());
    if (base >= 0x20 && base < 0x7F) {
        const int key = (base >= 'a' && base <= 'z') ? int(base - 'a' + 'A') : int(base);
        const int combined = QKeyCombination(mods, Qt::Key(key)).toCombined();
        if (combined != result.front()) result << combined;
    }
    return result;
}
template<typename T>
static const T* lookup(const QHash<int, T>& map, const QKeyEvent* event)
{
    for (int key : Keybinds::combinations(event)) {
        if (auto it = map.constFind(key); it != map.constEnd()) return &*it;
    }
    return nullptr;
}

Action Keybinds::match(const QKeyEvent* event) const
{
    const Action* a = lookup(m_byKey, event);
    return a ? *a : Action::None;
}

QString Keybinds::matchScript(const QKeyEvent* event) const
{
    const QString* s = lookup(m_scriptByKey, event);
    return s ? *s : QString();
}

QByteArray Keybinds::matchText(const QKeyEvent* event) const
{
    const QByteArray* b = lookup(m_textByKey, event);
    return b ? *b : QByteArray();
}

QByteArray Keybinds::unescape(const QString& text)
{
    QByteArray out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] != QLatin1Char('\\')) {
            out += QString(text[i]).toUtf8();
            continue;
        }
        if (++i >= text.size()) {
            return {};
        }
        auto hex = [&](int digits) -> std::optional<char32_t> {
            bool ok = false;
            const uint v = text.mid(i + 1, digits).toUInt(&ok, 16);
            if (!ok || text.mid(i + 1, digits).size() != digits) {
                return std::nullopt;
            }
            i += digits;
            return char32_t(v);
        };
        switch (text[i].unicode()) {
        case 'e':
            out += '\x1b';
            break;
        case 'r':
            out += '\r';
            break;
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case '\\':
            out += '\\';
            break;
        case 'x':
            if (const auto v = hex(2)) {
                out += char(*v);
            } else {
                return {};
            }
            break;
        case 'u':
            if (const auto v = hex(4)) {
                out += QString::fromUcs4(&*v, 1).toUtf8();
            } else {
                return {};
            }
            break;
        default:
            return {};
        }
    }
    return out;
}

QStringList Keybinds::bindingsOfScript(const QString& action) const
{
    QStringList out;
    for (auto it = m_scriptByKey.constBegin(); it != m_scriptByKey.constEnd(); ++it) {
        if (it.value() == action) {
            out.append(QKeySequence(QKeyCombination::fromCombined(it.key())).toString(QKeySequence::NativeText));
        }
    }
    out.sort();
    return out;
}

QStringList Keybinds::bindingsOf(Action action) const
{
    QStringList out;
    for (auto it = m_byKey.constBegin(); it != m_byKey.constEnd(); ++it) {
        if (it.value() == action) {
            out.append(QKeySequence(QKeyCombination::fromCombined(it.key())).toString(QKeySequence::NativeText));
        }
    }
    out.sort();
    return out;
}

} // namespace Afterglow
