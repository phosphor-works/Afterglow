// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Socket.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// The client of the "commands" socket target: `afterglow run`. It lists the
/// commands of script packages, runs one, passes its output through and exits
/// with its status. It finds the socket itself (findSocket), so a front end
/// only passes its arguments on.
namespace AfterglowProto::Run {

inline constexpr const char* Target = "commands";
/// Stdin goes to the terminal in parts of at most this many bytes of JSON
/// string, far below the router's 1 MiB request line.
inline constexpr std::size_t InputPartBytes = 512 * 1024;
/// The wait for an answer to list, input and cancel. A run waits without end:
/// the terminal limits it (Scripts.commandTimeout).
inline constexpr int ReplyTimeoutMs = 10000;

/// The terminal sends a run's output this many bytes ahead of what the client
/// acknowledged (CommandSocket::AckWindow). The client acknowledges in steps
/// of AckBytes, the event lines that it wrote, '\n' included.
inline constexpr std::size_t AckWindowBytes = 4 * 1024 * 1024;
inline constexpr std::size_t AckBytes = 256 * 1024;

/// The error code of a run for a script or command that does not exist.
inline constexpr const char* NoSuchCommand = "NO_SUCH_COMMAND";

/// Exit statuses besides the command's own.
inline constexpr int UsageError = 2; ///< a usage error or an unknown command, as in `afterglow ctl`
inline constexpr int Interrupted = 130; ///< SIGINT
inline constexpr int Terminated = 143; ///< SIGTERM
inline constexpr int BrokenPipe = 141; ///< stdout closed (SIGPIPE)

/// What the client reads from its process. Tests give their own.
struct Environment
{
    std::string program = "afterglow run"; ///< in messages
    EnvironmentLookup env = systemEnvironment; ///< for the socket and the pane
    std::string cwd; ///< empty: the working directory
    int input = 0; ///< stdin goes to a command only when this is not a terminal
    int output = 1;
    int error = 2;
};

/// Runs `afterglow run` with the arguments after "run". Returns the exit status.
int main(const std::vector<std::string>& args, const Environment& environment = {});

/// The bytes at the start of @p data that make the next stdin part: at most
/// InputPartBytes once written as a JSON string, and never a part of a UTF-8
/// sequence unless @p eof. 0 when more data must come first.
std::size_t inputPart(std::string_view data, bool eof);

} // namespace AfterglowProto::Run
