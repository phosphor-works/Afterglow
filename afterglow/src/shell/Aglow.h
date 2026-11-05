// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Afterglow::Aglow {

/// transfer, dnd, icat and clipboard: the commands that aglow runs.
bool handles(const char* command);
/// Runs the aglow that is in the same directory as this executable, with
/// argv[1] and the arguments after it. Returns only when it cannot start it.
int run(int argc, char** argv);

} // namespace Afterglow::Aglow
