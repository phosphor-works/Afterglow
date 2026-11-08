// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Fd.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace Aglow {

/// Each subcommand takes the arguments after its name and returns the exit
/// code. runRun ends the process instead by the signal that ended its run,
/// when one did.
int runTransfer(int argc, char** argv);
int runDnd(int argc, char** argv);
int runIcat(int argc, char** argv);
int runClipboard(int argc, char** argv);
int runRun(int argc, char** argv);
/// aglow remote-prune DIR: the probe of `afterglow ssh` runs it on a remote host.
int runRemotePrune(int argc, char** argv);

/// $HOME, or "/" without it. No passwd lookup: a static binary has no NSS.
std::string homeDirectory();
/// The working directory, as `aglow run` reads it too: empty when it has no path.
using AfterglowProto::currentDirectory;
/// Prints "<program> <command>: <message>" on stderr, with Cli::program()
/// ("aglow", or "afterglow" when `afterglow icat` runs it).
void report(std::string_view command, std::string_view message);
/// 512 B, 1.5 KiB, 20.0 MiB.
std::string humanSize(std::uint64_t bytes);
/// Reads all of @p fd; false on a read error.
bool readAll(int fd, std::string& out);

} // namespace Aglow
