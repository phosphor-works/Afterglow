// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowVt/Input.h>

#include <QByteArray>
#include <Qt>

#include <memory>

class QKeyEvent;

namespace Afterglow {

class ModifierRemap;

std::uint8_t mapModifiers(Qt::KeyboardModifiers mods);

/// Translates a Qt key press or release. @p textStorage keeps the UTF-8 text
/// alive for the returned event's string_view.
AfterglowVt::KeyEvent mapKeyEvent(const QKeyEvent* event, QByteArray& textStorage);

/// The character @p event's key types without modifiers on the active
/// layout, for matching key bindings (Keybinds::combinations). On a
/// Cyrillic or Greek layout it is the key's character on the first Latin
/// layout, as Qt does for Ctrl shortcuts. 0 when the key has none.
char32_t bindingBaseKey(const QKeyEvent* event);

/// A key that only changes the modifiers, as Qt names it.
bool isModifierKey(int key);

/// The modifiers after @p event. For a modifier key of the layout (Shift,
/// Ctrl, Alt or Super), its own bit is set on the press and clear on the
/// release, which Qt's report does not always have.
Qt::KeyboardModifiers modifiersAfter(const QKeyEvent& event);

/// @p event as Keybinds.remapModifiers makes it: its modifiers remapped, a
/// modifier key named as the modifier that it becomes when that is one, and
/// the key and text that the layout gives with the new modifiers. Null when
/// the remap changes nothing.
std::unique_ptr<QKeyEvent> remappedKeyEvent(const QKeyEvent& event, const ModifierRemap& remap);

/// @p key, the terminal's name of @p event's key, as the one modifier that
/// the key becomes, on the same side, as kitty reports it. Any other key as
/// it is.
AfterglowVt::Key remappedModifierKey(const QKeyEvent& event, AfterglowVt::Key key, const ModifierRemap& remap);

} // namespace Afterglow
