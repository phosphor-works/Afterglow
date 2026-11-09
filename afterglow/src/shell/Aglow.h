// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <string>

namespace Afterglow::Aglow {

/// A command that aglow runs, with the summary of aglow's own usage, for
/// afterglow --help. The usage is in tools/aglow/main.cpp, which cannot
/// include this header: CommandSocketTest checks that the two agree.
struct Command {
    const char* name;
    const char* summary;
};

/// The commands that aglow runs: afterglow COMMAND runs aglow COMMAND.
inline constexpr std::array<Command, 5> Commands { {
    { "run", "Run a command of a script in a running Afterglow" },
    { "transfer", "Copy files to and from the computer of the terminal" },
    { "dnd", "Drag files and data out of the terminal, and drop them in" },
    { "icat", "Show images in the terminal" },
    { "clipboard", "Read and write the clipboard, with any MIME type" },
} };
/// Whether this build has aglow (AFTERGLOW_BUILD_AGLOW).
bool built();
/// Whether @p command is the name of one of Commands.
bool handles(const char* command);
/// The program @p name in the directory of this executable: the aglow or
/// afterglow-mux of the same build or package, never another program of
/// that name on PATH. Empty with @p error when this executable cannot be
/// found.
std::string siblingExecutable(const char* name, std::string* error);
/// Runs the aglow that is in the same directory as this executable, with
/// argv[1] and the arguments after it. Returns only when it cannot start it,
/// or when this build has no aglow.
int run(int argc, char** argv);

} // namespace Afterglow::Aglow
