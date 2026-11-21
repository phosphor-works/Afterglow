// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace Afterglow {

/// Names Afterglow's folder for PhosphorPaths (`afterglow`, inside the suite
/// folder of each XDG root): its config, state and own data are there. Call it
/// at the top of main(), before anything reads a path.
void setAppDirectory();

/// The tree of the terminal shader packs, relative to the XDG data roots
/// (`phosphor-works/shaders/terminal`), the form QStandardPaths::locate takes.
QString shaderDataSubdir();

} // namespace Afterglow
