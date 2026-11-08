// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Remote.h>
#include <AfterglowProto/Socket.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
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
/// The wait for an answer to list, the connection included, and to input,
/// after the input went out.
/// Cancel and ack get none that the client waits for. A run waits without
/// end: the terminal limits it (Scripts.commandTimeout).
inline constexpr int ReplyTimeoutMs = 10000;
/// The wait of --complete, for the connection and the list together: a
/// shell's completion must not hang on an Afterglow that does not answer.
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

/// What the terminal answered to the query for the socket of an ssh session
/// (OSC 24569, AfterglowProto/Remote.h).
struct TerminalAnswer
{
    enum class Kind {
        /// The terminal was not asked: standard output is not the controlling
        /// terminal, or the client's process group is not in the foreground
        /// there (a pipeline, a background job, no terminal at all)
        None,
        Other, ///< the guard came alone, or nothing in time: not an Afterglow pane
        Reply, ///< answer holds the pane's reply
        Passthrough, ///< inside tmux, which passes nothing on while allow-passthrough is off
        Interrupted, ///< Ctrl+C while the client waited
    };
    Kind kind = Kind::None;
    Remote::Answer answer;
};
/// Asks the terminal, for at most @p timeoutMs.
using TerminalQuery = std::function<TerminalAnswer(int timeoutMs)>;

/// What the client reads from its process. Tests give their own.
struct Environment
{
    std::string program = "aglow run"; ///< in the usage and in messages
    EnvironmentLookup env = systemEnvironment; ///< for the socket and the pane
    std::string cwd; ///< empty: the working directory (currentDirectory())
    int input = 0; ///< stdin goes to a command only when this is not a terminal
    int output = 1;
    int error = 2;
    /// The wait for the answers to list and to input (ReplyTimeoutMs). The
    /// wait for an answer to input starts once the input went out.
    int replyTimeoutMs = ReplyTimeoutMs;
    /// When $AFTERGLOW_SOCKET is not set, or names no socket on this host,
    /// the client asks the terminal for the socket of its ssh session with
    /// this. Empty: it asks nothing (`aglow` gives its own, for /dev/tty).
    TerminalQuery askTerminal;
};

/// Runs `aglow run` with the arguments after "run". Returns the exit
/// status. On a remote host it finds the socket that `afterglow ssh`
/// forwarded through Environment::askTerminal, or in $AFTERGLOW_SOCKET when
/// that names a socket of the form of Remote::socketPath(), and uses the target
/// Remote::Target there. @p signal, when given, gets the signal that ended the run: SIGINT,
/// SIGTERM, or SIGPIPE for a closed standard output, else 0. The caller then
/// ends by that signal after its own cleanup, so that a shell sees it and a
/// loop of runs stops at Ctrl+C. A SIGPIPE that came ignored is not reported.
///
/// It acts on the whole process, so one thread at a time may call it. It
/// opens /dev/null on a standard descriptor that is closed, and leaves it
/// open: else the socket could take that number. While a command runs, it
/// has its own handlers for SIGINT and SIGTERM (unless they came ignored)
/// and ignores SIGPIPE. It puts the three back before it returns.
int main(const std::vector<std::string>& args, const Environment& environment = {}, int* signal = nullptr);

/// Cuts stdin into the parts that go to the terminal. Each part is at most
/// the limit once written as a JSON string, and never ends inside a UTF-8
/// sequence unless the input ended.
class InputParts
{
public:
    /// The quotes and the longest piece of a string, an escape such as
    /// \u0001. A smaller limit counts as this one: else no part could take
    /// that piece, and next() would never give a part.
    static constexpr std::size_t MinLimit = 8;

    explicit InputParts(std::size_t limit = InputPartBytes)
        : m_limit(std::max(limit, MinLimit))
    {
    }

    /// The bytes at the start of @p data that make the next part. Until
    /// @p eof, 0 while the part is not full: more data must come first.
    /// @p data is the caller's buffer, which grows at its end between two
    /// calls: each call reads only the bytes that came since the last one.
    std::size_t next(std::string_view data, bool eof);
    /// After the caller drops the part that next() gave from the front of
    /// its buffer.
    void restart()
    {
        m_at = 0;
        m_size = 2;
    }
    /// False once next() read bytes that are not UTF-8, and at the end of
    /// the data so far bytes that no later byte can make UTF-8. The wire
    /// would carry U+FFFD in their place, so the client refuses the input.
    bool text() const { return m_text; }

private:
    std::size_t m_limit;
    std::size_t m_at = 0; ///< where the bytes that next() did not read yet start
    std::size_t m_size = 2; ///< the written size of the bytes before m_at, the quotes included
    bool m_text = true;
};

/// The next part of @p data, as a new InputParts gives it.
std::size_t inputPart(std::string_view data, bool eof, std::size_t limit = InputPartBytes);

} // namespace AfterglowProto::Run
