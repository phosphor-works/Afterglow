// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow run: a command of a script in a running Afterglow. The client is in
// afterglow-proto. This front end names it, asks the terminal for the socket
// of an ssh session on /dev/tty, and ends by the signal that ended the run.

#include "Cli.h"
#include "Commands.h"
#include "Tty.h"

#include <AfterglowProto/Remote.h>
#include <AfterglowProto/Run.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace Aglow {

namespace {

namespace Remote = AfterglowProto::Remote;
using AfterglowProto::Run::TerminalAnswer;

/// tmux says that allow-passthrough is off for this pane. False when tmux
/// cannot tell: the query then goes, and the guard ends the wait.
bool passthroughOff()
{
    int out[2];
    if (::pipe2(out, O_CLOEXEC) != 0) return false;
    const char* argv[] = {"tmux", "display-message", "-p", "#{allow-passthrough}", nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int spawned = ::posix_spawnp(&pid, "tmux", &actions, nullptr, const_cast<char**>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    std::string text;
    if (spawned == 0) readAll(out[0], text);
    ::close(out[0]);
    if (spawned != 0) return false;
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && text == "off\n";
}

/// Asks the terminal on /dev/tty for the socket of its ssh session: OSC
/// 24569, then a query that every terminal answers, after the answer to the
/// first. That is DA1, or inside tmux a status report (DSR 5) that goes
/// through tmux with the query: tmux answers DA1 itself at once, before the
/// terminal can, and keeps the terminal's DA1 answer. Only when a standard
/// descriptor is a terminal, so that a run in a pipeline of a script asks
/// nothing. The terminal stays in its modes: no echo and no line editing
/// while it waits, and nothing else, so that a shell's completion can run
/// this between two keys.
TerminalAnswer askTerminal(int timeoutMs)
{
    if (!::isatty(STDIN_FILENO) && !::isatty(STDOUT_FILENO) && !::isatty(STDERR_FILENO)) return {};
    const char* tmuxVariable = std::getenv("TMUX");
    const bool tmux = tmuxVariable && *tmuxVariable;
    if (tmux && passthroughOff()) return {TerminalAnswer::Kind::Passthrough, {}};
    const int fd = ::open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return {};
    termios saved{};
    if (::tcgetattr(fd, &saved) != 0) {
        ::close(fd);
        return {};
    }
    termios quiet = saved;
    // Ctrl+C arrives as a byte, so that the modes come back before the exit.
    quiet.c_lflag &= ~tcflag_t(ECHO | ICANON | ISIG | IEXTEN);
    quiet.c_cc[VMIN] = 1;
    quiet.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSANOW, &quiet) != 0) {
        ::close(fd);
        return {};
    }
    const std::string query = tmux ? Remote::tmuxPassthrough(Remote::query() + "\x1b[5n") : Remote::query() + "\x1b[c";
    TerminalAnswer answer{TerminalAnswer::Kind::Other, {}};
    bool guard = false;
    if (AfterglowProto::writeAll(fd, query)) {
        InputParser parser;
        std::vector<Input> inputs;
        const long long deadline = nowMs() + timeoutMs;
        while (!guard && answer.kind != TerminalAnswer::Kind::Interrupted) {
            const long long left = deadline - nowMs();
            if (left <= 0) break;
            pollfd ready{fd, POLLIN, 0};
            if (::poll(&ready, 1, int(left)) < 0) {
                if (errno == EINTR) continue;
                break;
            }
            char bytes[4096];
            const ssize_t n = ::read(fd, bytes, sizeof bytes);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) break;
            inputs.clear();
            parser.feed(std::string_view(bytes, std::size_t(n)), inputs);
            for (const Input& input : inputs) {
                if (input.type == Input::Type::Osc && input.data.starts_with("24569;")) {
                    if (const auto reply = Remote::parseReply(std::string_view(input.data).substr(6)))
                        answer = {TerminalAnswer::Kind::Reply, *reply};
                } else if (input.type == Input::Type::Csi
                           && (tmux ? input.data == "0n" : input.data.starts_with('?') && input.data.ends_with('c'))) {
                    guard = true;
                } else if (input.type == Input::Type::Interrupt) {
                    answer = {TerminalAnswer::Kind::Interrupted, {}};
                }
            }
        }
    }
    // An answer that is late would reach the shell as typed text: without
    // the guard, the input that waits goes.
    ::tcsetattr(fd, guard ? TCSANOW : TCSAFLUSH, &saved);
    ::close(fd);
    return answer;
}

} // namespace

int runRun(int argc, char** argv)
{
    AfterglowProto::Run::Environment environment;
    // "afterglow run" when afterglow runs this aglow.
    environment.program = Cli::program() + " run";
    environment.askTerminal = askTerminal;
    int signal = 0;
    const int status = AfterglowProto::Run::main({argv, argv + argc}, environment, &signal);
    // A run that a signal ended ends by it: a shell's loop of runs then stops
    // at Ctrl+C. The status is for a signal that the process blocks.
    if (signal != 0) {
        std::signal(signal, SIG_DFL);
        std::raise(signal);
    }
    return status;
}

int runRemotePrune(int argc, char** argv)
{
    if (argc != 1 || std::string_view(argv[0]).starts_with('-')) {
        report("remote-prune", "Usage: " + Cli::program() + " remote-prune DIR\n"
                               "Removes the sockets in DIR that belong to you and refuse a connection: the ones that "
                               "sessions of afterglow ssh left when they ended.");
        return 2;
    }
    const AfterglowProto::Remote::Pruned pruned = AfterglowProto::Remote::prune(argv[0], ::getuid());
    if (!pruned.error.empty()) {
        report("remote-prune", pruned.error);
        return 1;
    }
    return 0;
}

} // namespace Aglow
