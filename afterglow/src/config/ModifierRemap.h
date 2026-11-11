// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <Qt>

#include <array>
#include <optional>

namespace Afterglow {

/// Keybinds.remapModifiers: one modifier acts as another, as kitty's
/// remap_modifiers does. The window applies it to each input event before
/// anything reads the event, so key bindings, mouse actions and the keys that
/// programs get all see the modifier that a key becomes.
class ModifierRemap
{
public:
    /// Reads kitty's syntax, "<from>:<to> ...", for example "ctrl:super
    /// super:ctrl". The source is one of shift, ctrl, alt and super (with
    /// kitty's aliases control, opt, option, cmd and command); the
    /// destination is one or more of them, joined by "+". The last rule for a
    /// source wins. An empty value remaps nothing. A value with an error
    /// gives nullopt and @p error, and remaps nothing, as in kitty.
    static std::optional<ModifierRemap> parse(const QString& text, QString* error = nullptr);

    bool isEmpty() const noexcept { return m_empty; }
    /// @p modifiers with each of the four remapped. Other flags (Keypad,
    /// GroupSwitch) stay.
    Qt::KeyboardModifiers apply(Qt::KeyboardModifiers modifiers) const noexcept;
    /// The single modifier that @p source becomes, or Qt::NoModifier when it
    /// becomes more than one. A modifier key reports itself as this one.
    Qt::KeyboardModifier singleTarget(Qt::KeyboardModifier source) const noexcept;

    bool operator==(const ModifierRemap&) const = default;

private:
    static constexpr std::array<Qt::KeyboardModifier, 4> Sources{Qt::ShiftModifier, Qt::ControlModifier, Qt::AltModifier,
                                                                  Qt::MetaModifier};
    /// What each of Sources becomes; each one itself without a rule.
    std::array<Qt::KeyboardModifiers, 4> m_targets{Qt::ShiftModifier, Qt::ControlModifier, Qt::AltModifier, Qt::MetaModifier};
    bool m_empty = true;
};

} // namespace Afterglow
