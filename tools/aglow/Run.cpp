// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow run: a command of a script in a running Afterglow. The client is in
// afterglow-proto. This front end names it, asks the terminal on /dev/tty
// for the socket of an ssh session, and ends by the signal that ended the
// run.

#include "Cli.h"
#include "Commands.h"
#include "Tty.h"

#include <AfterglowProto/Remote.h>
#include <AfterglowProto/Run.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iterator>
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

/// What tmux says of allow-passthrough for this pane.
enum class Passthrough {
    Off,
    Unknown, ///< on, or tmux cannot tell: the query goes, and the guard ends the wait
    Late, ///< tmux did not answer by the deadline: no time is left for the query
};

/// Asks tmux by @p deadline (nowMs()). tmux waits for its server, which may
/// not answer.
Passthrough passthrough(long long deadline)
{
    int out[2];
    if (::pipe2(out, O_CLOEXEC | O_NONBLOCK) != 0) return Passthrough::Unknown;
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
    // The end of tmux's output: tmux exits there.
    bool ended = false;
    while (spawned == 0 && !ended) {
        pollfd ready{out[0], POLLIN, 0};
        const int polled = ::poll(&ready, 1, int(std::max(0LL, deadline - nowMs())));
        if (polled < 0 && errno == EINTR) continue;
        if (polled <= 0) break;
        char bytes[256];
        const ssize_t n = ::read(out[0], bytes, sizeof bytes);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) {
            ended = true;
        } else {
            text.append(bytes, std::size_t(n));
        }
    }
    ::close(out[0]);
    if (spawned != 0) return Passthrough::Unknown;
    // A tmux that did not answer in time ends now, so that the wait for it
    // is short.
    if (!ended) ::kill(pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!ended) return Passthrough::Late;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && text == "off\n" ? Passthrough::Off : Passthrough::Unknown;
}

/// Whether the client may ask its terminal: standard output is the
/// controlling terminal, and the client's process group is in the
/// foreground there. A run that writes into a pipe or a file asks nothing:
/// the reader of `aglow run ... | less` reads the terminal too, and could
/// take the answer. A run in the background asks nothing: it would stop on
/// SIGTTOU with the terminal in its modes. A pipeline that writes into
/// aglow run, whose standard output stays the terminal, can still have a
/// program that reads the terminal (vipe, fzf): that one can take the answer.
bool mayAsk()
{
    const pid_t session = ::tcgetsid(STDOUT_FILENO);
    return session != -1 && session == ::getsid(0) && ::tcgetpgrp(STDOUT_FILENO) == ::getpgrp();
}

/// The signals that QuerySignals catches, and a bit for each in g_caught.
constexpr int Caught[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP};

constexpr int caughtBit(int number)
{
    for (std::size_t i = 0; i < std::size(Caught); ++i) {
        if (Caught[i] == number) return 1 << i;
    }
    return 0;
}

volatile std::sig_atomic_t g_caught = 0;

/// Each handler runs with the others blocked (sa_mask), so that none of the
/// bits is lost between the read and the write.
extern "C" void onQuerySignal(int number)
{
    g_caught = g_caught | caughtBit(number);
}

/// While the query holds the terminal in its modes, the signals that end or
/// stop the process are caught, and act after the modes are back. SIGTTIN
/// and SIGTTOU stay blocked: a group that left the foreground still puts the
/// modes back (tcsetattr goes on when SIGTTOU is blocked), and its read gets
/// EIO, not a stop.
class QuerySignals
{
public:
    QuerySignals()
    {
        sigset_t block;
        sigemptyset(&block);
        for (const int number : Caught) sigaddset(&block, number);
        sigaddset(&block, SIGTTIN);
        sigaddset(&block, SIGTTOU);
        ::sigprocmask(SIG_BLOCK, &block, &m_mask);
        // The waits let the caught signals in, unless the process blocked them already.
        m_wait = m_mask;
        sigaddset(&m_wait, SIGTTIN);
        sigaddset(&m_wait, SIGTTOU);
        g_caught = 0;
        struct sigaction action{};
        action.sa_handler = onQuerySignal;
        sigemptyset(&action.sa_mask);
        for (const int number : Caught) sigaddset(&action.sa_mask, number);
        for (std::size_t i = 0; i < std::size(Caught); ++i) {
            ::sigaction(Caught[i], nullptr, &m_old[i]);
            // A signal that came ignored stays ignored (SIGINT in a
            // background job of a shell without job control).
            if (m_old[i].sa_handler != SIG_IGN) ::sigaction(Caught[i], &action, nullptr);
        }
    }
    /// After the modes are back: each caught signal, still blocked, is
    /// raised again, and acts as it would have once the mask comes back.
    ~QuerySignals()
    {
        for (std::size_t i = 0; i < std::size(Caught); ++i) ::sigaction(Caught[i], &m_old[i], nullptr);
        const int caught = g_caught;
        g_caught = 0;
        for (const int number : Caught) {
            if (caught & caughtBit(number)) ::raise(number);
        }
        ::sigprocmask(SIG_SETMASK, &m_mask, nullptr);
    }
    QuerySignals(const QuerySignals&) = delete;
    QuerySignals& operator=(const QuerySignals&) = delete;

    const sigset_t* waitMask() const { return &m_wait; }
    bool caught() const { return g_caught != 0; }
    bool ignored(int number) const
    {
        const auto at = std::find(std::begin(Caught), std::end(Caught), number);
        return at != std::end(Caught) && m_old[std::size_t(at - std::begin(Caught))].sa_handler == SIG_IGN;
    }
    /// A caught SIGINT, which the query reports as Interrupted, so that the
    /// client says why it ends before it ends by the signal.
    bool takeInterrupt()
    {
        if (!(g_caught & caughtBit(SIGINT))) return false;
        g_caught = g_caught & ~caughtBit(SIGINT);
        return true;
    }

private:
    sigset_t m_mask{};
    sigset_t m_wait{};
    struct sigaction m_old[std::size(Caught)]{};
};

enum class Wait { Ready, Timeout, Signal, Failed };

/// Waits until @p fd has @p events, @p deadline (nowMs()) passes or a
/// caught signal comes.
Wait waitFor(int fd, short events, long long deadline, const QuerySignals& signals)
{
    const long long ms = deadline - nowMs();
    if (ms <= 0) return Wait::Timeout;
    pollfd ready{fd, events, 0};
    const timespec timeout{time_t(ms / 1000), long(ms % 1000) * 1000000L};
    const int n = ::ppoll(&ready, 1, &timeout, signals.waitMask());
    if (n > 0) return Wait::Ready;
    if (n == 0) return Wait::Timeout;
    return errno == EINTR ? Wait::Signal : Wait::Failed;
}

/// Writes the query by @p deadline. A terminal that holds its input back
/// (XOFF) ends the write at the deadline, and a signal ends it at once.
bool writeQuery(int fd, std::string_view bytes, long long deadline, const QuerySignals& signals)
{
    while (!bytes.empty()) {
        if (signals.caught()) return false;
        const ssize_t n = ::write(fd, bytes.data(), bytes.size());
        if (n > 0) {
            bytes.remove_prefix(std::size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return false;
        const Wait wait = waitFor(fd, POLLOUT, deadline, signals);
        if (wait == Wait::Timeout || wait == Wait::Failed) return false;
    }
    return true;
}

/// Asks the terminal on /dev/tty for the socket of its ssh session: OSC
/// 24569, then a query that every terminal answers, after the answer to the
/// first. That is DA1, or inside tmux a status report (DSR 5) that goes
/// through tmux with the query: tmux answers DA1 itself at once, before the
/// terminal can, and keeps the terminal's DA1 answer. Only when mayAsk().
///
/// The client reads all that the terminal sends up to the guard's answer,
/// so keys typed meanwhile, or typed ahead and not read yet, are lost.
/// Without the guard's answer, by the deadline or after a failed write, the
/// terminal's queues are flushed: a late answer would reach the shell as
/// typed text, and a query held back (XOFF) would get one. The terminal
/// keeps its signal keys, and Ctrl+C cancels as SIGINT does. No echo and no
/// line editing while it waits, and nothing else, so that this can run
/// between two keys. Every exit puts the modes back, signals included:
/// QuerySignals holds them until then.
TerminalAnswer askTerminal(int timeoutMs)
{
    const long long deadline = nowMs() + timeoutMs;
    if (!mayAsk()) return {};
    const char* tmuxVariable = std::getenv("TMUX");
    const bool tmux = tmuxVariable && *tmuxVariable;
    // Before the modes change: tmux gets the process's own signal mask.
    if (tmux) {
        switch (passthrough(deadline)) {
        case Passthrough::Off:
            return {TerminalAnswer::Kind::Passthrough, {}};
        case Passthrough::Late:
            // A query without time to wait for its answer would leave the
            // answer to the shell.
            return {TerminalAnswer::Kind::Other, {}};
        case Passthrough::Unknown:
            break;
        }
    }
    const int fd = ::open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return {};
    QuerySignals signals;
    termios saved{};
    if (::tcgetattr(fd, &saved) != 0) {
        ::close(fd);
        return {};
    }
    termios quiet = saved;
    quiet.c_lflag &= ~tcflag_t(ECHO | ICANON | IEXTEN);
    // Ctrl+C, Ctrl+\ and Ctrl+Z keep the answers that are on their way.
    quiet.c_lflag |= NOFLSH;
    quiet.c_cc[VMIN] = 1;
    quiet.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSANOW, &quiet) != 0) {
        ::close(fd);
        return {};
    }
    const std::string query = tmux ? Remote::tmuxPassthrough(Remote::query() + "\x1b[5n") : Remote::query() + "\x1b[c";
    TerminalAnswer answer{TerminalAnswer::Kind::Other, {}};
    bool guard = false;
    bool interrupted = false;
    if (writeQuery(fd, query, deadline, signals)) {
        InputParser parser;
        std::vector<Input> inputs;
        // Up to the guard after a signal too: its answer is on its way.
        while (!guard) {
            const Wait wait = waitFor(fd, POLLIN, deadline, signals);
            if (wait == Wait::Timeout || wait == Wait::Failed) break;
            if (wait == Wait::Signal) continue;
            char bytes[4096];
            const ssize_t n = ::read(fd, bytes, sizeof bytes);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) break;
            inputs.clear();
            parser.feed(std::string_view(bytes, std::size_t(n)), inputs);
            for (const Input& input : inputs) {
                const auto payload = input.type == Input::Type::Osc ? Remote::oscPayload(input.data) : std::nullopt;
                if (payload) {
                    if (const auto reply = Remote::parseReply(*payload)) answer = {TerminalAnswer::Kind::Reply, *reply};
                } else if (input.type == Input::Type::Csi
                           && (tmux ? input.data == "0n" : input.data.starts_with('?') && input.data.ends_with('c'))) {
                    guard = true;
                } else if (input.type == Input::Type::Interrupt && !signals.ignored(SIGINT)) {
                    // Ctrl+C as a key code of kitty's keyboard protocol.
                    interrupted = true;
                }
            }
        }
    }
    if (!guard) ::tcflush(fd, TCIOFLUSH);
    ::tcsetattr(fd, TCSANOW, &saved);
    ::close(fd);
    if (signals.takeInterrupt() || interrupted) return {TerminalAnswer::Kind::Interrupted, {}};
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
    const std::string usage = "Usage: " + Cli::program() + " remote-prune DIR\n"
                              "Removes the sockets in DIR that belong to you and refuse a connection: the ones that "
                              "sessions of afterglow ssh left when they ended.\n";
    if (argc == 1 && (std::string_view(argv[0]) == "--help" || std::string_view(argv[0]) == "-h")) {
        std::fputs(usage.c_str(), stdout);
        return 0;
    }
    if (argc != 1 || std::string_view(argv[0]).starts_with('-')) {
        std::fputs(usage.c_str(), stderr);
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
