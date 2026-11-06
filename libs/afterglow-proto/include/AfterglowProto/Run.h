// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Socket.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// The client of the "commands" socket target: `aglow run`, which `afterglow
/// run` runs. It lists the commands of scripts, runs one, passes its output
/// through and exits with its status. It finds the socket itself
/// (findSocket), so a front end only passes its arguments on.
namespace AfterglowProto::Run {

inline constexpr const char* Target = "commands";
/// Stdin goes to the terminal in parts of at most this many bytes of JSON
/// string, far below the router's 1 MiB request line.
inline constexpr std::size_t InputPartBytes = 512 * 1024;
/// The wait for an answer to list, the connection included, and to input.
/// Cancel and ack get none that the client waits for. A run waits without
/// end: the terminal limits it (Scripts.commandTimeout).
inline constexpr int ReplyTimeoutMs = 10000;
/// The wait of --complete, for the connection and the list together: a
/// shell's completion must not hang on an afterglow that does not answer.
inline constexpr int CompleteTimeoutMs = 1000;

/// The terminal sends the output of a connection's runs this many bytes ahead
/// of what the client acknowledged, for all the runs together
/// (CommandSocket::AckWindow). The client acknowledges the event lines that it
/// wrote, '\n' included, once they make AckBytes over all its runs: each run's
/// own count, which is all that the terminal takes for a run.
inline constexpr std::size_t AckWindowBytes = 4 * 1024 * 1024;
inline constexpr std::size_t AckBytes = 256 * 1024;

/// The error code of a run for a script or command that does not exist.
inline constexpr const char* NoSuchCommand = "NO_SUCH_COMMAND";
/// The error code of input past Scripts.commandStdinLimit. The terminal keeps
/// none of the run's input, and the run cannot start.
inline constexpr const char* InputTooLarge = "INPUT_TOO_LARGE";

/// The arguments of a command, as the terminal limits them (ScriptHost).
/// The client refuses more before it connects, as a usage error.
inline constexpr std::size_t MaxArgs = 256;
inline constexpr std::size_t MaxArgLength = 4096; ///< UTF-16 code units of one argument

/// Exit statuses besides the command's own.
inline constexpr int UsageError = 2; ///< a usage error, an unknown command or refused input, as in `afterglow ctl`
/// 128 + the signal that ended the run, for when ending by the signal itself
/// does not end the process (main()).
inline constexpr int Interrupted = 130; ///< SIGINT
inline constexpr int Terminated = 143; ///< SIGTERM
inline constexpr int BrokenPipe = 141; ///< stdout closed (SIGPIPE)

/// What the client reads from its process. Tests give their own.
struct Environment
{
    std::string program = "aglow run"; ///< in the usage and in messages
    EnvironmentLookup env = systemEnvironment; ///< for the socket and the pane
    std::string cwd; ///< empty: the working directory
    int input = 0; ///< stdin goes to a command only when this is not a terminal
    int output = 1;
    int error = 2;
};

/// Runs `aglow run` with the arguments after "run". Returns the exit
/// status. @p signal, when given, gets the signal that ended the run: SIGINT,
/// SIGTERM, or SIGPIPE for a closed standard output, else 0. The caller then
/// ends by that signal after its own cleanup, so that a shell sees it and a
/// loop of runs stops at Ctrl+C. A SIGPIPE that came ignored is not reported.
int main(const std::vector<std::string>& args, const Environment& environment = {}, int* signal = nullptr);

/// The bytes at the start of @p data that make the next stdin part: at most
/// InputPartBytes once written as a JSON string, and never a part of a UTF-8
/// sequence unless @p eof. Until @p eof, 0 while the part is not full: more
/// data must come first.
std::size_t inputPart(std::string_view data, bool eof);

} // namespace AfterglowProto::Run
