// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Keybinds.h"

#include "../XkbKeyboard.h"

#include <QKeyEvent>
#include <QKeySequence>

namespace Afterglow {

namespace {

struct ActionInfo
{
    Action action;
    const char* token;
    std::initializer_list<const char*> defaults;
};

const ActionInfo Actions[] = {
    {Action::SplitRight, "split_right", {"Ctrl+Shift+E"}},
    {Action::SplitDown, "split_down", {"Ctrl+Shift+O"}},
    {Action::ClosePane, "close_pane", {"Ctrl+Shift+W"}},
    {Action::NewWindow, "new_window", {"Ctrl+Shift+N"}},
    {Action::NewTab, "new_tab", {"Ctrl+Shift+T"}},
    {Action::CloseTab, "close_tab", {"Ctrl+Shift+Q"}},
    {Action::NextTab, "next_tab", {"Ctrl+Tab", "Ctrl+PgDown"}},
    {Action::PreviousTab, "previous_tab", {"Ctrl+Shift+Tab", "Ctrl+PgUp"}},
    {Action::Copy, "copy", {"Ctrl+Shift+C"}},
    {Action::Paste, "paste", {"Ctrl+Shift+V"}},
    {Action::FocusLeft, "focus_left", {"Ctrl+Shift+Left"}},
    {Action::FocusRight, "focus_right", {"Ctrl+Shift+Right"}},
    {Action::FocusUp, "focus_up", {"Ctrl+Shift+Up"}},
    {Action::FocusDown, "focus_down", {"Ctrl+Shift+Down"}},
    {Action::EqualizeSplits, "equalize_splits", {"Ctrl+Shift+="}},
    {Action::FontBigger, "font_bigger", {"Ctrl+=", "Ctrl++"}},
    {Action::FontSmaller, "font_smaller", {"Ctrl+-"}},
    {Action::FontReset, "font_reset", {"Ctrl+0"}},
    {Action::ScrollPageUp, "scroll_page_up", {"Shift+PgUp"}},
    {Action::ScrollPageDown, "scroll_page_down", {"Shift+PgDown"}},
    {Action::ScrollTop, "scroll_top", {"Shift+Home"}},
    {Action::ScrollBottom, "scroll_bottom", {"Shift+End"}},
    {Action::JumpPromptUp, "jump_prompt_up", {"Ctrl+Shift+Z"}},
    {Action::JumpPromptDown, "jump_prompt_down", {"Ctrl+Shift+X"}},
    {Action::Search, "search", {"Ctrl+Shift+F"}},
    {Action::CommandPalette, "command_palette", {"Ctrl+Shift+P"}},
    {Action::ReloadConfig, "reload_config", {"Ctrl+Shift+R"}},
    {Action::OpenConfig, "open_config", {"Ctrl+Shift+,"}},
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

void Keybinds::bind(Action action, const QString& sequence, QStringList* errors)
{
    if (sequence.trimmed().isEmpty()) {
        return;
    }
    const QKeySequence seq = QKeySequence::fromString(sequence, QKeySequence::PortableText);
    if (seq.isEmpty() || seq[0].key() == Qt::Key_unknown) {
        if (errors) {
            errors->append(QStringLiteral("cannot parse key \"%1\" for %2").arg(sequence, token(action)));
        }
        return;
    }
    if (seq.count() > 1 && errors) {
        errors->append(QStringLiteral("\"%1\": only single key combinations are supported").arg(sequence));
    }
    const QKeyCombination combo(seq[0].keyboardModifiers() & RelevantMods, seq[0].key());
    m_byKey.insert(combo.toCombined(), action);
}

Keybinds Keybinds::fromMap(const QVariantMap& map, QStringList* errors)
{
    Keybinds kb;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        bool known = false;
        for (const ActionInfo& a : Actions) {
            if (it.key() == QLatin1String(a.token)) {
                known = true;
            }
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

Action Keybinds::match(const QKeyEvent* event) const
{
    const Qt::KeyboardModifiers mods = event->modifiers() & RelevantMods;
    const int key = event->key();
    if (auto it = m_byKey.constFind(QKeyCombination(mods, Qt::Key(key)).toCombined()); it != m_byKey.constEnd()) {
        return *it;
    }
    // Shift changes the reported key on punctuation (Shift+= reports '+'), so
    // also try the key's unshifted character from the layout.
    const char32_t base = XkbKeyboard::instance().unshiftedCodepoint(event->nativeScanCode());
    if (base >= 0x20 && base < 0x7F) {
        const int baseKey = (base >= 'a' && base <= 'z') ? int(base - 'a' + 'A') : int(base);
        if (auto it = m_byKey.constFind(QKeyCombination(mods, Qt::Key(baseKey)).toCombined());
            it != m_byKey.constEnd()) {
            return *it;
        }
    }
    return Action::None;
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
