// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <Qt>

#include <array>
#include <optional>

namespace Afterglow {

/// Keybinds.remapModifiers: one modifier acts as another, as kitty's
/// remap_modifiers does. The window applies it to each key, mouse and wheel
/// event before anything reads the event, so key bindings, the link modifier,
/// mouse selection and the keys that programs get all see the modifier that a
/// key becomes.
class ModifierRemap
{
public:
    /// Reads kitty's syntax, "<from>:<to> ...", for example "ctrl:super
    /// super:ctrl". Each name is shift, ctrl, alt or super, or one of kitty's
    /// aliases (control, opt, option, cmd, command and the symbols ⇧ ⌃ ⌥ ⌘),
    /// in any letter case. The source is one modifier, and the destination is
    /// one or more joined by "+". The last rule for a source wins. An empty
    /// value remaps nothing. A value with an error gives nullopt and @p error,
    /// and remaps nothing, as in kitty.
    static std::optional<ModifierRemap> parse(const QString& text, QString* error = nullptr);

    bool isEmpty() const noexcept { return m_targets == Identity; }
    /// @p modifiers with each of the four remapped, all at once: a swap does
    /// not chain. Other flags (Keypad, GroupSwitch) stay.
    Qt::KeyboardModifiers apply(Qt::KeyboardModifiers modifiers) const noexcept;
    /// The one modifier that a rule makes of @p source. Qt::NoModifier when no
    /// rule names @p source, when its rule gives several modifiers, and for a
    /// flag that is not shift, ctrl, alt or super. A modifier key reports
    /// itself as this one.
    Qt::KeyboardModifier singleTarget(Qt::KeyboardModifier source) const noexcept;

    bool operator==(const ModifierRemap&) const = default;

private:
    static constexpr std::array<Qt::KeyboardModifier, 4> Sources{Qt::ShiftModifier, Qt::ControlModifier, Qt::AltModifier,
                                                                  Qt::MetaModifier};
    static constexpr std::array<Qt::KeyboardModifiers, 4> Identity{Qt::ShiftModifier, Qt::ControlModifier,
                                                                    Qt::AltModifier, Qt::MetaModifier};
    /// What each of Sources becomes: itself without a rule.
    std::array<Qt::KeyboardModifiers, 4> m_targets = Identity;
};

} // namespace Afterglow
