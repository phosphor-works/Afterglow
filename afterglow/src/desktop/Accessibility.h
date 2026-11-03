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
/// A script's dock drew again: assistive technology hears of new text and
/// of items that came, went or changed their names.
void dockAccessibilityChanged(const DockArea*);
/// Forgets the accessible object of a dock, before the dock goes.
void releaseDockAccessibility(const DockArea*);
/// The panes or docks that a window shows may have changed: assistive
/// technology hears to read its children again when they did.
void windowAccessibilityChanged(TerminalWindow*);
}
