// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Fd.h>
#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Json.h>
#include <AfterglowProto/Random.h>
#include <AfterglowProto/Run.h>
#include <AfterglowProto/Text.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <optional>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace AfterglowProto::Run {

namespace {

const char* const Description = R"(
Runs a command of a script in a running Afterglow (Control.enabled and
Scripts.enabled in its config). The command acts on --pane N, else on the
pane that this shell runs in, else on the focused pane.

  -n, --no-stdin    do not pass standard input to the command
  --pane N          the pane that the command acts on, a pane ID from 1
  -h, --help        show this help

A command that reads standard input gets it only when standard input is not
a terminal. Input that is not UTF-8 text is refused, and so is input over
Scripts.commandStdinLimit.

For a shell's completion, --complete prints the script IDs, and
--complete ID the commands of one script as COMMAND<TAB>SUMMARY lines.
--complete --all prints the commands of all scripts as
ID<TAB>COMMAND<TAB>SUMMARY lines. They leave out the names that a shell
would expand.

The exit status is the status of the command. Exit status 1 is an error
outside the command, such as no Afterglow or a lost connection. Exit status
2 is a usage error, an unknown command or refused input, and 124 a command
that ran out of time (Scripts.commandTimeout). SIGINT and SIGTERM cancel the
command, and so does a closed standard output (SIGPIPE). Then
%1 ends by that signal: a shell reports 130, 143 or 141.
)";

/// Where the descriptions of the usage lines start.
constexpr std::size_t UsageColumn = 46;

/// The usage, under the name of the program: "aglow run", or "afterglow
/// run" when afterglow runs aglow.
std::string usageText(const std::string& program)
{
    const auto line = [](std::string head, std::string_view text) {
        head.resize(std::max(head.size() + 2, UsageColumn), ' ');
        return head + std::string(text) + "\n";
    };
    // %1 in the description stands for the program.
    std::string description;
    const std::string_view text = Description;
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t mark = std::min(text.find("%1", at), text.size());
        description += text.substr(at, mark - at);
        if (mark < text.size()) description += program;
        at = mark + 2;
    }
    return line("Usage: " + program, "list the commands of the scripts")
        + line("       " + program + " ID --help", "the commands of one script")
        + "       " + program + " [-n] [--pane N] ID COMMAND [ARGS...]\n"
        + line({}, "run a command, print its output and exit with its status")
        + line("       " + program + " --complete [ID | --all]", "names for a shell's completion") + description;
}

/// The cells of printable() text on a terminal, by cellWidth(): the columns
/// of the listing line up unless the terminal's tables disagree.
std::size_t columns(std::string_view text)
{
    std::size_t cells = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        char32_t cp = 0;
        if (!nextUtf8(text, i, cp)) {
            // printable() leaves no such byte: it would show as one '?'.
            ++i;
            ++cells;
            continue;
        }
        cells += std::size_t(cellWidth(cp));
    }
    return cells;
}

/// The UTF-16 code units of @p text, as the terminal counts an argument.
/// Nothing when it is not UTF-8: the wire would carry U+FFFD in place of
/// its bytes, and the command would get another word than the one typed.
std::optional<std::size_t> utf16Length(std::string_view text)
{
    std::size_t units = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        char32_t cp = 0;
        if (!nextUtf8(text, i, cp)) return std::nullopt;
        units += cp >= 0x10000 ? 2 : 1;
    }
    return units;
}

/// Whether @p tail, where nextUtf8() found no character, starts a sequence
/// that more bytes can complete. A byte that starts none, or a byte that the
/// lead byte rules out (an overlong form, a surrogate, past U+10FFFF), is
/// never UTF-8 whatever follows.
bool utf8Prefix(std::string_view tail)
{
    const auto lead = static_cast<unsigned char>(tail[0]);
    const std::size_t length = lead >= 0xC2 && lead <= 0xDF ? 2 : lead >= 0xE0 && lead <= 0xEF ? 3 : lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
    if (length == 0 || tail.size() >= length) return false;
    for (std::size_t k = 1; k < tail.size(); ++k) {
        const auto c = static_cast<unsigned char>(tail[k]);
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (k == 1) {
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED) high = 0x9F;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F;
        }
        if (c < low || c > high) return false;
    }
    return true;
}

/// A script ID or command name that a shell's completion can take as it is:
/// it expands nothing in it.
bool plainName(std::string_view name)
{
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

static_assert(Interrupted == 128 + SIGINT && Terminated == 128 + SIGTERM && BrokenPipe == 128 + SIGPIPE);

// The self-pipe of the signal handler: the run loop waits on it.
int g_wake[2] = {-1, -1};
volatile std::sig_atomic_t g_signal = 0;

/// Opens /dev/null on a closed stdin, stdout or stderr. Else the socket
/// would take that number, and the command's input or output would be the
/// connection itself.
void keepStandardDescriptors()
{
    for (int fd = 0; fd <= 2; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1 || errno != EBADF) continue;
        // The lowest free number, which is this one: the ones below are open.
        const int opened = ::open("/dev/null", O_RDWR);
        if (opened > 2) ::close(opened);
    }
}

struct Command
{
    std::string script;
    std::string name;
    std::string summary;
    std::string usage;
    bool input = false;
};

class Client
{
public:
    explicit Client(const Environment& environment)
        : m_env(environment)
        , m_socket(findSocket(environment.env))
    {
    }

    /// Prints @p message, which can hold the terminal's or a script's text.
    /// The wake pipe ends a write that a reader holds back when a signal
    /// comes, while the run watches for signals.
    int fail(std::string_view message, int status = 1) const
    {
        writeAll(m_error, m_env.program + ": " + printable(message) + "\n", g_wake[0]);
        return status;
    }

    int usage(std::string_view message) const
    {
        writeAll(m_error, m_env.program + ": " + printable(message) + "\n\n" + usageText(m_env.program), g_wake[0]);
        return UsageError;
    }

    /// Prints @p text on standard output. 0, else 1 with the reason.
    int print(std::string_view text) const
    {
        if (writeAll(m_output, text, g_wake[0])) return 0;
        const int failure = errno;
        return fail(std::string("cannot write standard output: ") + std::strerror(failure));
    }

    /// The exit status of a run that signal @p number ended. main() reports
    /// the signal unless @p report is false.
    int signalled(int number, bool report = true)
    {
        if (report) m_signal = number;
        return 128 + number;
    }
    /// After the wake descriptor ended a wait.
    int interrupted()
    {
        if (!g_signal) return fail("cannot watch for signals: the pipe failed");
        return signalled(g_signal);
    }
    int signal() const { return m_signal; }

    /// Connects and reads the commands, both in @p timeoutMs. Reports its own
    /// errors unless @p quiet.
    std::optional<std::vector<Command>> commands(bool quiet, int timeoutMs)
    {
        if (!resolve(quiet, timeoutMs)) return std::nullopt;
        if (m_socket.path.empty()) {
            if (!quiet) fail(m_socket.error);
            return std::nullopt;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        IpcConnection::Error error;
        if (!m_ipc.connect(m_socket.path, &error, timeoutMs)) {
            if (!quiet) connectFailed(error, timeoutMs);
            return std::nullopt;
        }
        // What the connection left of the wait: a shell's completion waits
        // for both.
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        auto list = m_ipc.call(m_target, "list", {}, int(std::max<std::chrono::milliseconds::rep>(left.count(), 0)), &error);
        if (!list) {
            if (!quiet) report(error, timeoutMs);
            return std::nullopt;
        }
        std::vector<Command> out;
        for (const Json::Value& item : list->toArray()) {
            out.push_back({item["script"].toString(), item["command"].toString(), item["summary"].toString(),
                           item["usage"].toString(), item["stdin"].toBool()});
        }
        return out;
    }

    void report(const IpcConnection::Error& error, int timeoutMs = ReplyTimeoutMs) const
    {
        if (error.code == "NO_SUCH_TARGET") {
            fail("the running Afterglow takes no commands (Control.enabled or Scripts.enabled is off)");
        } else if (error.timedOut) {
            fail("Afterglow did not answer in " + std::to_string(timeoutMs / 1000) + " seconds");
        } else if (error.code.empty()) {
            fail("lost Afterglow: " + error.message);
        } else {
            fail(error.message);
        }
    }

    /// After the connection closed by itself.
    int lost() const
    {
        const std::string& reason = m_ipc.closeReason();
        return fail("lost Afterglow: " + (reason.empty() ? std::string("the connection closed") : reason));
    }

    IpcConnection& ipc() { return m_ipc; }
    /// The target of the socket: Target, or Remote::Target on a remote host.
    const std::string& target() const { return m_target; }
    bool remote() const { return m_target == Remote::Target; }
    const Environment& env() const { return m_env; }
    /// The descriptors that the client writes standard output and error to:
    /// those of env(), or during a run descriptions of its own (OwnOutput).
    int output() const { return m_output; }
    int error() const { return m_error; }
    void writeTo(int output, int error)
    {
        m_output = output;
        m_error = error;
    }
    /// $AFTERGLOW_PANE_ID whenever $AFTERGLOW_SOCKET is set, else 0: the pane
    /// that this shell runs in when the socket is of its Afterglow.
    std::int64_t pane() const { return m_socket.pane; }

private:
    /// The socket of $AFTERGLOW_SOCKET when it is on this host, else the one
    /// that the terminal names for its ssh session, else the display's. False
    /// after a message when there is none and the terminal said why.
    bool resolve(bool quiet, int timeoutMs)
    {
        if (m_socket.own && isSocket(m_socket.path)) return true;
        if (!m_env.askTerminal) return true;
        const TerminalAnswer answer = m_env.askTerminal(std::min(timeoutMs, Remote::GuardTimeoutMs));
        std::string reason;
        switch (answer.kind) {
        case TerminalAnswer::Kind::None:
        case TerminalAnswer::Kind::Other:
            return true;
        case TerminalAnswer::Kind::Interrupted:
            if (!quiet) fail("interrupted while it asked the terminal");
            m_signal = SIGINT;
            return false;
        case TerminalAnswer::Kind::Passthrough:
            reason = "inside tmux, aglow asks the terminal through tmux, which passes nothing on while allow-passthrough "
                     "is off (tmux set -g allow-passthrough on)";
            break;
        case TerminalAnswer::Kind::Reply:
            if (!answer.answer.path.empty()) {
                if (isSocket(answer.answer.path)) {
                    m_socket = {answer.answer.path, false, 0, {}};
                    m_target = Remote::Target;
                    return true;
                }
                // After a second ssh hop: the terminal's session is with the first host.
                reason = "the socket of this ssh session, " + answer.answer.path
                    + ", is on another host: run the command on the host that afterglow ssh connected to";
                break;
            }
            switch (answer.answer.error) {
            case Remote::Error::Off:
                reason = "the Afterglow of this ssh session forwards no socket for its commands (it needs "
                         "Terminal.sshRemoteCommands, afterglow ssh, and aglow on this host when it connected)";
                break;
            case Remote::Error::Forward:
                reason = "the ssh server refuses to forward the socket of the commands (AllowStreamLocalForwarding in "
                         "its sshd_config)";
                break;
            case Remote::Error::Version:
                reason = "this aglow speaks another protocol than the Afterglow of this ssh session (aglow --version), "
                         "which forwards no socket then";
                break;
            case Remote::Error::Length:
                reason = "the path of the socket on this host would be over " + std::to_string(Remote::MaxPathBytes)
                    + " bytes: a shorter XDG_RUNTIME_DIR or HOME here makes it fit";
                break;
            }
            break;
        }
        // An Afterglow of this host runs the commands still, through the
        // display's socket.
        if (isSocket(m_socket.path)) return true;
        // A pane of this host with no socket: the local lookup says why.
        const bool off = answer.kind == TerminalAnswer::Kind::Reply && answer.answer.path.empty()
            && answer.answer.error == Remote::Error::Off;
        if (off && !m_socket.error.empty()) return true;
        if (!quiet) fail(reason);
        return false;
    }

    static bool isSocket(const std::string& path)
    {
        struct stat info{};
        return !path.empty() && ::stat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode);
    }

    /// Says why the connection failed, by what the lookup found.
    void connectFailed(const IpcConnection::Error& error, int timeoutMs) const
    {
        // A queue of new connections that stays full: there is an Afterglow,
        // and it does not take the connection.
        if (error.timedOut) {
            report(error, timeoutMs);
        } else if (remote()) {
            // The forward is there, and the shared ssh connection behind it is not.
            fail("the socket of this ssh session does not answer (" + error.message + ")");
        } else if (error.noServer) {
            fail(std::string(!m_socket.own   ? "no running Afterglow on this display"
                             : m_socket.pane ? "the Afterglow that runs this pane has exited"
                                             : "no Afterglow at $AFTERGLOW_SOCKET")
                 + " (" + error.message + ")");
        } else {
            fail(std::string("cannot connect to ")
                 + (!m_socket.own   ? "the Afterglow of this display"
                    : m_socket.pane ? "the Afterglow that runs this pane"
                                    : "the Afterglow at $AFTERGLOW_SOCKET")
                 + ": " + error.message);
        }
    }

    const Environment& m_env;
    SocketLookup m_socket;
    std::string m_target = Target;
    IpcConnection m_ipc;
    int m_output = m_env.output;
    int m_error = m_env.error;
    int m_signal = 0;
};

std::string listing(const std::vector<Command>& commands, std::string_view only)
{
    const auto head = [](const Command& c) { return printable(c.usage.empty() ? c.name : c.name + " " + c.usage); };
    std::size_t width = 0;
    for (const Command& c : commands) {
        if (only.empty() || c.script == only) width = std::max(width, columns(head(c)));
    }
    std::string out;
    std::string script;
    for (const Command& c : commands) {
        if (!only.empty() && c.script != only) continue;
        if (c.script != script) {
            script = c.script;
            out += printable(script) + "\n";
        }
        const std::string text = head(c);
        out += "  " + text + std::string(width - columns(text), ' ') + "  " + printable(c.summary) + "\n";
    }
    return out;
}

extern "C" void onSignal(int number)
{
    const int saved = errno;
    g_signal = number;
    const char byte = 1;
    [[maybe_unused]] const ssize_t ignored = ::write(g_wake[1], &byte, 1);
    errno = saved;
}

/// SIGINT and SIGTERM end the wait and cancel the run, SIGPIPE turns into
/// EPIPE. All three come back as they were at the end.
class Signals
{
public:
    Signals()
    {
        if (::pipe2(g_wake, O_CLOEXEC | O_NONBLOCK) != 0) return;
        struct sigaction action{};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        // No SA_RESTART: a read of stdin must return when a signal comes. A
        // signal that came ignored stays ignored (SIGINT in a background job
        // of a shell without job control), as in QuitSignals.cpp.
        for (const auto& [number, old] : {std::pair{SIGINT, &m_int}, std::pair{SIGTERM, &m_term}}) {
            ::sigaction(number, nullptr, old);
            if (old->sa_handler != SIG_IGN) ::sigaction(number, &action, nullptr);
        }
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        ::sigaction(SIGPIPE, &ignore, &m_pipe);
        m_installed = true;
    }
    ~Signals() { restore(); }
    Signals(const Signals&) = delete;
    Signals& operator=(const Signals&) = delete;

    /// Puts the dispositions back, and returns the signal that came while
    /// the handlers were in place, else 0. A signal after that acts as it
    /// did before: none is lost between the last look and the return.
    int restore()
    {
        if (!m_installed) return 0;
        m_installed = false;
        ::sigaction(SIGINT, &m_int, nullptr);
        ::sigaction(SIGTERM, &m_term, nullptr);
        ::sigaction(SIGPIPE, &m_pipe, nullptr);
        const int number = g_signal;
        ::close(g_wake[0]);
        ::close(g_wake[1]);
        g_wake[0] = g_wake[1] = -1;
        g_signal = 0;
        return number;
    }

    bool installed() const { return m_installed; }
    /// A process that ignores SIGPIPE gets EPIPE and ends with a status.
    bool pipeIgnored() const { return !(m_pipe.sa_flags & SA_SIGINFO) && m_pipe.sa_handler == SIG_IGN; }

private:
    struct sigaction m_int{}, m_term{}, m_pipe{};
    bool m_installed = false;
};

/// Sends stdin in parts until it ends. Returns 0, or the exit status to stop
/// with.
int sendInput(Client& client, const std::string& run)
{
    const int input = client.env().input;
    std::string buffer;
    InputParts parts;
    bool eof = false;
    while (true) {
        // A part goes as soon as it is full, so input over the terminal's
        // limit is refused early.
        std::size_t size = parts.next(buffer, eof);
        while (parts.text() && !eof && size == 0) {
            // A signal that comes between two reads must not wait for the
            // producer's next data.
            if (g_signal) return client.interrupted();
            // The socket too: a terminal that went away shows while stdin
            // stays idle.
            pollfd fds[3] = {{input, POLLIN, 0}, {g_wake[0], POLLIN, 0}, {client.ipc().fd(), POLLIN, 0}};
            if (::poll(fds, 3, -1) < 0 && errno != EINTR) {
                return client.fail(std::string("cannot read standard input: ") + std::strerror(errno));
            }
            if (g_signal || fds[1].revents) return client.interrupted();
            if (fds[2].revents) {
                // Before the run the terminal sends only answers, and this
                // client waits for none: the frame is dropped, a close ends
                // the run.
                Json::Value frame;
                const auto status = client.ipc().next(frame, 0, g_wake[0]);
                if (status == IpcConnection::Read::Woken) return client.interrupted();
                if (status == IpcConnection::Read::Closed) return client.lost();
            }
            if (!fds[0].revents) continue;
            char chunk[65536];
            const ssize_t n = ::read(input, chunk, sizeof chunk);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n < 0) return client.fail(std::string("cannot read standard input: ") + std::strerror(errno));
            if (n == 0) eof = true;
            buffer.append(chunk, std::size_t(n));
            size = parts.next(buffer, eof);
        }
        // As an argument that is not UTF-8: the wire would carry U+FFFD in
        // place of its bytes.
        if (!parts.text()) return client.fail("standard input is not UTF-8 text", UsageError);
        if (g_signal) return client.interrupted();
        const bool last = eof && size == buffer.size();
        IpcConnection::Error error;
        const auto accepted = client.ipc().call(client.target(), "input", {run, buffer.substr(0, size), last}, ReplyTimeoutMs, &error, g_wake[0]);
        if (!accepted) {
            if (error.interrupted) return client.interrupted();
            // Over Scripts.commandStdinLimit: the run cannot start, as with a
            // word that it does not take.
            if (error.code == InputTooLarge) return client.fail(error.message, UsageError);
            client.report(error);
            return 1;
        }
        buffer.erase(0, size);
        parts.restart();
        if (last) return 0;
    }
}

/// The directory that the command runs in, empty for the pane's. Nothing
/// when it is not UTF-8: the wire would carry U+FFFD in place of its bytes,
/// and name another directory.
std::optional<std::string> workingDirectory(const Environment& env)
{
    std::string path = env.cwd.empty() ? currentDirectory() : env.cwd;
    if (!utf16Length(path)) return std::nullopt;
    return path;
}

std::string requestTooLong()
{
    return "the arguments and the working directory make a request over " + std::to_string(IpcConnection::MaxRequestBytes >> 20)
        + " MiB";
}

/// The run, while @p signals watch for signals. @p request holds the
/// arguments of the run call. @p unknown, when not empty, is the message
/// for a command that the terminal does not know.
int watched(Client& client, const Signals& signals, const std::string& run, Json::Array request, bool sendStdin,
            std::string_view unknown)
{
    if (sendStdin) {
        if (const int status = sendInput(client, run)) return status;
    }
    IpcConnection::Error error;
    const auto id = client.ipc().send(client.target(), "run", std::move(request), &error, g_wake[0]);
    if (!id) {
        if (error.interrupted) return client.interrupted();
        client.report(error);
        return 1;
    }
    // Without waiting: after a signal the send gives up when it would
    // block, and the connection that closes at the exit cancels the run too.
    auto cancel = [&] {
        IpcConnection::Error ignored;
        client.ipc().send(client.target(), "cancel", {run}, &ignored, g_wake[0]);
    };
    // What this client wrote of the event lines of its connection's runs and
    // has not acknowledged: the terminal sends AckWindowBytes ahead at most,
    // for all of them together. With one run on the connection, its count is
    // that total. A client of several runs acknowledges each run's count once
    // their total reaches AckBytes: each run alone may never get there.
    std::size_t written = 0;
    while (true) {
        // A signal ends the run before a frame that waited is printed, and
        // before a connection that a cut ack closed reads as lost.
        if (g_signal) {
            cancel();
            return client.interrupted();
        }
        Json::Value frame;
        std::size_t bytes = 0;
        const auto status = client.ipc().next(frame, -1, g_wake[0], &bytes);
        if (status == IpcConnection::Read::Woken) {
            cancel();
            return client.interrupted();
        }
        if (status != IpcConnection::Read::Frame) return client.lost();
        if (IpcConnection::answers(frame, *id)) {
            const auto result = IpcConnection::answer(std::move(frame), &error);
            if (!result) {
                if (error.code == NoSuchCommand) return client.fail(unknown.empty() ? std::string_view(error.message) : unknown, UsageError);
                client.report(error);
                return 1;
            }
            // A status that a process cannot exit with is no status of the command.
            const auto exit = (*result)["exit"].toInteger();
            return exit && *exit >= 0 && *exit <= 255 ? int(*exit) : 1;
        }
        const std::string& type = frame["type"].toString();
        const std::string& event = frame["signal"].toString();
        const Json::Array& eventArgs = frame["args"].toArray();
        if (type != "event" || frame["target"].toString() != client.target() || eventArgs.size() != 2 || eventArgs[0].toString() != run)
            continue;
        const bool out = event == "stdout";
        if (out || event == "stderr") {
            // The wake pipe ends a write that a reader holds back when a signal comes.
            if (!writeAll(out ? client.output() : client.error(), eventArgs[1].toString(), g_wake[0])) {
                const int failed = errno;
                if (g_signal) {
                    cancel();
                    return client.interrupted();
                }
                // A closed stdout ends the run as SIGPIPE ends the writer of
                // a pipeline. A stderr that fails loses its text and nothing
                // else.
                if (out) {
                    cancel();
                    if (failed == EPIPE) return client.signalled(SIGPIPE, !signals.pipeIgnored());
                    return client.fail(std::string("cannot write standard output: ") + std::strerror(failed));
                }
            }
        }
        written += bytes;
        if (written >= AckBytes) {
            // A failure shows at the next frame: a signal or a closed connection.
            IpcConnection::Error ignored;
            client.ipc().send(client.target(), "ack", {run, std::int64_t(written)}, &ignored, g_wake[0]);
            written = 0;
        }
    }
}

/// For a run, the client's standard output and error as open file
/// descriptions of its own, with O_NONBLOCK, when they are a pipe or a
/// terminal. A blocking write that a reader holds back cannot be ended by a
/// signal that came just before it, and the wake check before a write leaves
/// that moment open. A write that would block now returns, and writeAll then
/// waits for room or the wake. O_NONBLOCK on descriptors 1 and 2 themselves
/// would change the description that the shell and other processes share.
/// A file or a socket keeps the shared one: a file does not hold a write
/// back, and a socket cannot be opened again.
class OwnOutput
{
public:
    explicit OwnOutput(Client& client)
        : m_client(client)
        , m_output(open(client.env().output))
        , m_error(client.env().error == client.env().output ? -1 : open(client.env().error))
    {
        const int output = m_output >= 0 ? m_output : client.env().output;
        const int error = client.env().error == client.env().output ? output
                          : m_error >= 0                             ? m_error
                                                                     : client.env().error;
        client.writeTo(output, error);
    }
    ~OwnOutput()
    {
        m_client.writeTo(m_client.env().output, m_client.env().error);
        if (m_output >= 0) ::close(m_output);
        if (m_error >= 0) ::close(m_error);
    }
    OwnOutput(const OwnOutput&) = delete;
    OwnOutput& operator=(const OwnOutput&) = delete;

private:
    static int open(int fd)
    {
        struct stat info{};
        if (::fstat(fd, &info) != 0 || !(S_ISFIFO(info.st_mode) || ::isatty(fd))) return -1;
        // A pipe with no reader left gives ENXIO: the shared one then gets
        // its EPIPE, as before.
        const std::string path = "/proc/self/fd/" + std::to_string(fd);
        return ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    }

    Client& m_client;
    int m_output;
    int m_error;
};

/// Runs @p run, whose run call has the arguments @p request. @p sendStdin
/// and @p unknown as in watched().
int runCommand(Client& client, const std::string& run, Json::Array request, bool sendStdin, std::string_view unknown = {})
{
    Signals signals;
    if (!signals.installed()) return client.fail(std::string("cannot watch for signals: ") + std::strerror(errno));
    const OwnOutput output(client);
    const int status = watched(client, signals, run, std::move(request), sendStdin, unknown);
    // A signal that came after the last look of the run ends the process as
    // it would without the handlers: a loop of runs stops at Ctrl+C.
    if (const int number = signals.restore()) return client.signalled(number);
    return status;
}

std::optional<std::int64_t> paneNumber(std::string_view text)
{
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value < 1 || value > 0xFFFFFFFFLL) return std::nullopt;
    return value;
}

/// For shell completion: the script IDs, "COMMAND\tSUMMARY" of one script,
/// or "ID\tCOMMAND\tSUMMARY" of all scripts with --all, which names no
/// script: a script ID does not start with '-'. Quiet and quick, as a shell
/// waits for it. Only plain names, which a shell's completion takes as
/// words and expands nothing in.
int complete(Client& client, const std::vector<std::string>& args)
{
    if (args.size() > 2) return UsageError;
    const bool all = args.size() == 2 && args[1] == "--all";
    const auto commands = client.commands(true, CompleteTimeoutMs);
    if (!commands) return 1;
    std::string out;
    std::string last;
    for (const Command& c : *commands) {
        if (!plainName(c.script)) continue;
        if (all) {
            if (plainName(c.name)) out += c.script + "\t" + c.name + "\t" + printable(c.summary) + "\n";
        } else if (args.size() == 1) {
            if (c.script != last) out += c.script + "\n";
        } else if (c.script == args[1] && plainName(c.name)) {
            out += c.name + "\t" + printable(c.summary) + "\n";
        }
        last = c.script;
    }
    // Without a message: a shell would show it in the middle of the line.
    return writeAll(client.env().output, out) ? 0 : 1;
}

int start(Client& client, const std::vector<std::string>& args)
{
    const Environment& environment = client.env();
    if (!args.empty() && args[0] == "--complete") return complete(client, args);

    bool readInput = true;
    std::optional<std::int64_t> pane;
    std::size_t at = 0;
    for (; at < args.size(); ++at) {
        const std::string& arg = args[at];
        if (arg == "-h" || arg == "--help") {
            return client.print(usageText(environment.program));
        } else if (arg == "-n" || arg == "--no-stdin") {
            readInput = false;
        } else if (arg == "--pane") {
            if (++at == args.size()) return client.usage("--pane needs a pane ID");
            pane = paneNumber(args[at]);
            if (!pane) return client.usage("--pane takes a pane ID from 1");
        } else if (arg == "--") {
            ++at;
            break;
        } else if (arg.size() > 1 && arg[0] == '-') {
            return client.usage("unknown option " + arg);
        } else {
            break;
        }
    }
    // Usage errors that need no Afterglow come before the connection.
    if (at == args.size() && at > 0) return client.usage("which script?");
    const bool running = at + 1 < args.size() && args[at + 1] != "--help" && args[at + 1] != "-h";
    std::string run;
    Json::Array request;
    if (running) {
        if (args.size() - at - 2 > MaxArgs) return client.usage("a command takes at most " + std::to_string(MaxArgs) + " arguments");
        for (std::size_t i = at + 2; i < args.size(); ++i) {
            const auto length = utf16Length(args[i]);
            if (!length) return client.usage("argument " + std::to_string(i - at - 1) + " is not UTF-8 text");
            if (*length > MaxArgLength) {
                return client.usage("argument " + std::to_string(i - at - 1) + " is over " + std::to_string(MaxArgLength) + " UTF-16 units");
            }
        }
        const auto directory = workingDirectory(environment);
        if (!directory) return client.fail("the working directory is not UTF-8 text");
        const auto id = randomHex(16);
        if (!id) return client.fail("the kernel gave no random bytes for the run ID");
        run = *id;
        Json::Array arguments;
        for (std::size_t i = at + 2; i < args.size(); ++i) arguments.emplace_back(args[i]);
        request = Json::Array{run, args[at], args[at + 1], std::move(arguments), *directory, pane.value_or(client.pane())};
        // A run call that the router would refuse: else the client would
        // wait for the list, and all of the input would go, for a run that
        // cannot start.
        if (!IpcConnection::fits(Target, "run", request)) return client.fail(requestTooLong(), UsageError);
    }

    const auto commands = client.commands(false, ReplyTimeoutMs);
    if (!commands) return client.signal() ? client.signalled(client.signal()) : 1;
    // A remote host has no pane IDs of the terminal's: the run acts on the
    // pane of the ssh session, which --pane can name too.
    if (running && client.remote() && !pane) request[5] = std::int64_t(0);
    if (at == args.size()) {
        const std::string out = listing(*commands, {});
        return client.print(out.empty() ? "No script has commands.\n" : out);
    }
    const std::string& script = args[at];
    const bool known = std::any_of(commands->begin(), commands->end(), [&](const Command& c) { return c.script == script; });
    const std::string noScript = "no script " + script + " with commands (" + environment.program + " lists them)";
    if (!running) {
        if (!known) return client.fail(noScript, UsageError);
        // ID --help asks for the commands. ID alone is a usage error: the
        // reason, then the commands that it can take.
        if (at + 1 < args.size()) return client.print(listing(*commands, script));
        client.fail("which command of " + script + "?");
        writeAll(environment.error, listing(*commands, script));
        return UsageError;
    }
    const std::string& name = args[at + 1];
    const auto command = std::find_if(commands->begin(), commands->end(),
                                      [&](const Command& c) { return c.script == script && c.name == name; });
    if (command == commands->end()) {
        // The terminal has the last word. The list leaves out a disabled
        // script, and only the refusal of its run says why. A command that
        // it does not know gets the message here, which says where to look.
        const std::string unknown = known ? "no command " + name + " in " + script + " (" + environment.program + " " + script
                + " --help lists them)"
                                          : noScript;
        return runCommand(client, run, std::move(request), false, unknown);
    }
    // Stdin only when it is not a terminal: typed at a prompt, the command
    // would wait for its input until Ctrl+D. A stdin that is no terminal and
    // does not end (`ssh host cmd` with no -n) needs -n.
    return runCommand(client, run, std::move(request), readInput && command->input && ::isatty(environment.input) != 1);
}

} // namespace

std::size_t InputParts::next(std::string_view data, bool eof)
{
    while (m_at < data.size()) {
        const std::size_t start = m_at;
        std::size_t end = start;
        char32_t cp = 0;
        if (!nextUtf8(data, end, cp)) {
            // A sequence cut at the end of what came so far may complete
            // later. Bytes that cannot are refused now: a stdin that stays
            // open would hold the run back.
            if (!eof && utf8Prefix(data.substr(start))) return 0;
            m_text = false;
        }
        const std::size_t cost = Json::nextStringPiece(data, m_at);
        if (m_size + cost > m_limit) {
            m_at = start;
            return start;
        }
        m_size += cost;
    }
    return eof ? data.size() : 0;
}

std::size_t inputPart(std::string_view data, bool eof, std::size_t limit)
{
    return InputParts(limit).next(data, eof);
}

int main(const std::vector<std::string>& args, const Environment& environment, int* signal)
{
    keepStandardDescriptors();
    Client client(environment);
    const int status = start(client, args);
    if (signal) *signal = client.signal();
    return status;
}

} // namespace AfterglowProto::Run
