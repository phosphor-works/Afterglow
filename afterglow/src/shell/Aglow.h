// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

namespace Afterglow::Aglow {

/// The commands that aglow runs: afterglow COMMAND runs aglow COMMAND.
inline constexpr std::array<const char*, 5> Commands { "run", "transfer", "dnd", "icat", "clipboard" };
/// Whether this build has aglow (AFTERGLOW_BUILD_AGLOW).
bool built();
/// Whether @p command is one of Commands.
bool handles(const char* command);
/// Runs the aglow that is in the same directory as this executable, with
/// argv[1] and the arguments after it. Returns only when it cannot start it,
/// or when this build has no aglow.
int run(int argc, char** argv);

} // namespace Afterglow::Aglow
