// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Aglow {

/// Each subcommand takes the arguments after its name and returns the exit code.
int runTransfer(int argc, char** argv);
int runDnd(int argc, char** argv);
int runIcat(int argc, char** argv);
int runClipboard(int argc, char** argv);

/// $HOME, or "/" without it. No passwd lookup: a static binary has no NSS.
std::string homeDirectory();
/// The working directory; empty when it has no path (it was deleted).
std::string currentDirectory();
/// Prints "<program> <command>: <message>" on stderr, with Cli::program()
/// ("aglow", or "afterglow" when `afterglow icat` runs it).
void report(std::string_view command, std::string_view message);
/// 512 B, 1.5 KiB, 20.0 MiB.
std::string humanSize(std::uint64_t bytes);
/// Reads all of @p fd; false on a read error.
bool readAll(int fd, std::string& out);

} // namespace Aglow
