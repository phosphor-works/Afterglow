// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowVt/Input.h>

#include <QByteArray>
#include <Qt>

class QKeyEvent;

namespace Afterglow {

std::uint8_t mapModifiers(Qt::KeyboardModifiers mods);

/// Translates a Qt key press or release. @p textStorage keeps the UTF-8 text
/// alive for the returned event's string_view.
AfterglowVt::KeyEvent mapKeyEvent(const QKeyEvent* event, QByteArray& textStorage);

/// The character @p event's key types without modifiers on the active
/// layout, for matching key bindings (Keybinds::combinations). On a
/// Cyrillic or Greek layout it is the key's character on the first Latin
/// layout, as Qt does for Ctrl shortcuts. 0 when the key has none.
char32_t bindingBaseKey(const QKeyEvent* event);

} // namespace Afterglow
