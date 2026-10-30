// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
class QQuickItem;
class QWindow;
namespace Afterglow {
class TerminalWindow;
void installTerminalAccessibility();
void installChromeAccessibility(QQuickItem*, QWindow*);
void updateTerminalAccessibility(TerminalWindow*);
}
