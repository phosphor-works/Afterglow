// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
class QQuickItem;
class QWindow;
namespace Afterglow {
class DockArea;
class TerminalWindow;
void installTerminalAccessibility();
void installChromeAccessibility(QQuickItem*, QWindow*);
void updateTerminalAccessibility(TerminalWindow*);
/// A script's dock shows new text: assistive technology reads it again.
void dockAccessibilityChanged(const DockArea*);
/// Forgets the accessible object of a dock, before the dock goes.
void releaseDockAccessibility(const DockArea*);
}
