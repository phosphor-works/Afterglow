// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class QDialog;

namespace Afterglow {

class Pane;

/// Makes @p dialog transient for the window that shows @p pane, and keeps
/// it over the pane: when that window goes, the dialog moves to the window
/// that the pane moved to, or is rejected when the pane goes with it. Call
/// it before the dialog shows (open(), not exec(): the native KDE dialog
/// moves by a hide and a show, which would end the loop of exec()). Nothing
/// happens when no window shows the pane.
void keepDialogOverPane(QDialog* dialog, const Pane* pane);

} // namespace Afterglow
