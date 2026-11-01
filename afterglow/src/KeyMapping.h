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

} // namespace Afterglow
