// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

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
#include <unistd.h>

namespace AfterglowProto::Run {

namespace {

const char* const Usage = R"(usage: afterglow run                          list the commands of the scripts
       afterglow run ID --help                the commands of one script
       afterglow run [-n] [--pane N] ID COMMAND [ARGS...]
                                              run a command, print its output and exit with its status

Runs a command of a script in a running afterglow (Control.enabled and
Scripts.enabled in its config). The command acts on --pane N, else on the
pane that this shell runs in, else on the focused pane.

  -n, --no-stdin    do not pass standard input to the command
  --pane N          the pane that the command acts on

A command that reads standard input gets it only when standard input is not
a terminal. Input over Scripts.commandStdinLimit is refused.

Exit status 2 is a usage error, an unknown command or refused input, and 124
a command that ran out of time (Scripts.commandTimeout). SIGINT and SIGTERM
cancel the command, and so does a closed standard output (SIGPIPE). Then
afterglow run ends by that signal: a shell reports 130, 143 or 141.
)";

/// Text from a script or the terminal, safe to print: no control character
/// can reach the terminal that shows it as an escape sequence.
std::string printable(std::string_view text)
{
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        char32_t cp = 0;
        if (!nextUtf8(text, i, cp)) {
            out += '?';
            i = start + 1;
        } else if (isControl(cp)) {
            out += '?';
        } else {
            out.append(text.substr(start, i - start));
        }
    }
    return out;
}

/// The columns of printable() text, one for each code point. A wide
/// character takes two, which this library cannot tell without the
/// terminal's Unicode tables: the listing is then off by one column.
std::size_t columns(std::string_view text)
{
    return std::size_t(std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
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

/// 0, or the errno of the write that failed. EINTR: a signal came while the
/// write waited for a reader that does not read, and the caller checks the
/// signal first.
int writeAll(int fd, std::string_view data)
{
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n > 0) {
            data.remove_prefix(std::size_t(n));
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (g_signal) return EINTR;
            // A descriptor that another process made non-blocking waits for
            // room here. The poll ends on room, an error or a signal: the
            // next write tells which.
            if (errno != EINTR) {
                pollfd fds[2] = {{fd, POLLOUT, 0}, {g_wake[0], POLLIN, 0}};
                ::poll(fds, 2, -1);
            }
            continue;
        }
        return n < 0 ? errno : EIO;
    }
    return 0;
}

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
    int fail(std::string_view message, int status = 1) const
    {
        writeAll(m_env.error, m_env.program + ": " + printable(message) + "\n");
        return status;
    }

    int usage(std::string_view message) const
    {
        writeAll(m_env.error, m_env.program + ": " + printable(message) + "\n\n" + Usage);
        return UsageError;
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
        if (m_socket.path.empty()) {
            if (!quiet) fail(m_socket.error);
            return std::nullopt;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        IpcConnection::Error error;
        if (!m_ipc.connect(m_socket.path, &error, timeoutMs)) {
            if (!quiet) {
                const char* const which = !m_socket.own ? "no running afterglow on this display ("
                    : m_socket.pane ? "the afterglow that runs this pane has exited ("
                                    : "no afterglow at $AFTERGLOW_SOCKET (";
                fail(which + error.message + ")");
            }
            return std::nullopt;
        }
        // What the connection left of the wait: a shell's completion waits
        // for both.
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        auto list = m_ipc.call(Target, "list", {}, int(std::max<std::chrono::milliseconds::rep>(left.count(), 0)), &error);
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
            fail("the running afterglow takes no commands (Control.enabled or Scripts.enabled is off)");
        } else if (error.timedOut) {
            fail("afterglow did not answer in " + std::to_string(timeoutMs / 1000) + " seconds");
        } else if (error.code.empty()) {
            fail("lost afterglow: " + error.message);
        } else {
            fail(error.message);
        }
    }

    IpcConnection& ipc() { return m_ipc; }
    const Environment& env() const { return m_env; }
    /// The pane that this shell runs in, of the afterglow that it talks to, else 0.
    std::int64_t pane() const { return m_socket.pane; }

private:
    const Environment& m_env;
    SocketLookup m_socket;
    IpcConnection m_ipc;
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
        // signal that came ignored stays ignored (nohup, a background job of
        // a shell without job control), as in QuitSignals.cpp.
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
    ~Signals()
    {
        if (!m_installed) return;
        ::sigaction(SIGINT, &m_int, nullptr);
        ::sigaction(SIGTERM, &m_term, nullptr);
        ::sigaction(SIGPIPE, &m_pipe, nullptr);
        ::close(g_wake[0]);
        ::close(g_wake[1]);
        g_wake[0] = g_wake[1] = -1;
        g_signal = 0;
    }
    Signals(const Signals&) = delete;
    Signals& operator=(const Signals&) = delete;

    bool installed() const { return m_installed; }
    int fd() const { return g_wake[0]; }
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
    bool eof = false;
    while (true) {
        // A part goes as soon as it is full, so input over the terminal's
        // limit is refused early.
        std::size_t size = inputPart(buffer, eof);
        while (!eof && size == 0) {
            // A signal that comes between two reads must not wait for the
            // producer's next data.
            if (g_signal) return client.interrupted();
            pollfd fds[2] = {{input, POLLIN, 0}, {g_wake[0], POLLIN, 0}};
            if (::poll(fds, 2, -1) < 0 && errno != EINTR) {
                return client.fail(std::string("cannot read standard input: ") + std::strerror(errno));
            }
            if (g_signal || fds[1].revents) return client.interrupted();
            if (!fds[0].revents) continue;
            char chunk[65536];
            const ssize_t n = ::read(input, chunk, sizeof chunk);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n < 0) return client.fail(std::string("cannot read standard input: ") + std::strerror(errno));
            if (n == 0) eof = true;
            buffer.append(chunk, std::size_t(n));
            size = inputPart(buffer, eof);
        }
        if (g_signal) return client.interrupted();
        const bool last = eof && size == buffer.size();
        IpcConnection::Error error;
        const auto accepted = client.ipc().call(Target, "input", {run, buffer.substr(0, size), last}, ReplyTimeoutMs, &error, g_wake[0]);
        if (!accepted) {
            if (error.interrupted) return client.interrupted();
            // Over Scripts.commandStdinLimit: the run cannot start, as with a
            // word that it does not take.
            if (error.code == InputTooLarge) return client.fail(error.message, UsageError);
            client.report(error);
            return 1;
        }
        buffer.erase(0, size);
        if (last) return 0;
    }
}

/// The directory that the command runs in, empty for the pane's.
std::optional<std::string> workingDirectory(const Environment& env)
{
    if (!env.cwd.empty()) return env.cwd;
    std::string path(4096, '\0');
    while (!::getcwd(path.data(), path.size())) {
        if (errno != ERANGE || path.size() > (1u << 20)) return std::string(); // removed, or too long: the pane's
        path.resize(path.size() * 2);
    }
    path.resize(std::strlen(path.c_str()));
    // The wire would carry U+FFFD in place of its bytes: another directory.
    if (!utf16Length(path)) return std::nullopt;
    return path;
}

int runCommand(Client& client, const Command& command, const std::vector<std::string>& args, const std::string& cwd,
               std::int64_t pane, bool readInput)
{
    const auto run = randomHex(16);
    if (!run) return client.fail("the kernel gave no random bytes for the run ID");
    Signals signals;
    if (!signals.installed()) return client.fail(std::string("cannot watch for signals: ") + std::strerror(errno));
    if (readInput && command.input) {
        if (const int status = sendInput(client, *run)) return status;
    }
    Json::Array arguments;
    for (const std::string& arg : args) arguments.emplace_back(arg);
    IpcConnection::Error error;
    const auto id = client.ipc().send(Target, "run", {*run, command.script, command.name, std::move(arguments), cwd, pane},
                                      &error, signals.fd());
    if (!id) {
        if (error.interrupted) return client.interrupted();
        // Too many or too long arguments for one request: a usage error.
        if (error.code == IpcConnection::RequestTooLong) return client.fail("the arguments are too long for one request (1 MiB)", UsageError);
        client.report(error);
        return 1;
    }
    // Without waiting: after a signal the send gives up when it would
    // block, and the connection that closes at the exit cancels the run too.
    auto cancel = [&] {
        IpcConnection::Error ignored;
        client.ipc().send(Target, "cancel", {*run}, &ignored, signals.fd());
    };
    // What this client wrote of the event lines of its connection's runs and
    // has not acknowledged: the terminal sends AckWindowBytes ahead at most,
    // for all of them together. With one run on the connection, its count is
    // that total. A client of several runs acknowledges each run's count once
    // their total reaches AckBytes: each run alone may never get there.
    std::size_t written = 0;
    while (true) {
        Json::Value frame;
        std::size_t bytes = 0;
        const auto status = client.ipc().next(frame, -1, signals.fd(), &bytes);
        if (status == IpcConnection::Read::Woken) {
            cancel();
            return client.interrupted();
        }
        if (status != IpcConnection::Read::Frame) {
            const std::string& reason = client.ipc().closeReason();
            return client.fail("lost afterglow: " + (reason.empty() ? std::string("the connection closed") : reason));
        }
        if (IpcConnection::answers(frame, *id)) {
            const auto result = IpcConnection::answer(std::move(frame), &error);
            if (!result) {
                if (error.code == NoSuchCommand) return client.fail(error.message, UsageError);
                client.report(error);
                return 1;
            }
            const auto exit = (*result)["exit"].toInteger();
            return exit && *exit >= 0 && *exit <= 255 ? int(*exit) : 1;
        }
        const std::string& type = frame["type"].toString();
        const std::string& event = frame["signal"].toString();
        const Json::Array& eventArgs = frame["args"].toArray();
        if (type != "event" || frame["target"].toString() != Target || eventArgs.size() != 2
            || eventArgs[0].toString() != *run)
            continue;
        const bool out = event == "stdout";
        if (out || event == "stderr") {
            if (const int failed = writeAll(out ? client.env().output : client.env().error, eventArgs[1].toString())) {
                // A signal while a reader held the write back.
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
            client.ipc().send(Target, "ack", {*run, std::int64_t(written)}, &ignored, signals.fd());
            written = 0;
        }
    }
}

std::optional<std::int64_t> paneNumber(std::string_view text)
{
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value < 1 || value > 0xFFFFFFFFLL) return std::nullopt;
    return value;
}

int start(Client& client, const std::vector<std::string>& args)
{
    const Environment& environment = client.env();
    if (!args.empty() && (args[0] == "-h" || args[0] == "--help")) {
        writeAll(environment.output, Usage);
        return 0;
    }
    if (!args.empty() && args[0] == "--complete") {
        // For shell completion: script IDs, or "COMMAND\tSUMMARY" of one.
        // Quiet and quick, as a shell waits for it. Only plain names, which
        // a shell's completion takes as words and expands nothing in.
        if (args.size() > 2) return UsageError;
        const auto commands = client.commands(true, CompleteTimeoutMs);
        if (!commands) return 1;
        std::string out;
        std::string last;
        for (const Command& c : *commands) {
            if (!plainName(c.script)) continue;
            if (args.size() == 1 && c.script != last) out += c.script + "\n";
            if (args.size() == 2 && c.script == args[1] && plainName(c.name)) out += c.name + "\t" + printable(c.summary) + "\n";
            last = c.script;
        }
        writeAll(environment.output, out);
        return 0;
    }

    bool readInput = true;
    std::optional<std::int64_t> pane;
    std::size_t at = 0;
    for (; at < args.size(); ++at) {
        const std::string& arg = args[at];
        if (arg == "-n" || arg == "--no-stdin") {
            readInput = false;
        } else if (arg == "--pane") {
            if (++at == args.size()) return client.usage("--pane needs a number");
            pane = paneNumber(args[at]);
            if (!pane) return client.usage("--pane takes a pane number");
        } else if (arg == "--") {
            ++at;
            break;
        } else if (arg.size() > 1 && arg[0] == '-') {
            return client.usage("unknown option " + arg);
        } else {
            break;
        }
    }
    // Usage errors that need no afterglow come before the connection.
    if (at == args.size() && at > 0) return client.usage("which script?");
    const bool running = at + 1 < args.size() && args[at + 1] != "--help" && args[at + 1] != "-h";
    std::string cwd;
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
        cwd = *directory;
    }

    const auto commands = client.commands(false, ReplyTimeoutMs);
    if (!commands) return 1;
    if (at == args.size()) {
        const std::string out = listing(*commands, {});
        writeAll(environment.output, out.empty() ? "No script has commands.\n" : out);
        return 0;
    }
    const std::string& script = args[at];
    const bool known = std::any_of(commands->begin(), commands->end(), [&](const Command& c) { return c.script == script; });
    if (!known) return client.fail("no script " + script + " with commands (afterglow run lists them)", UsageError);
    if (!running) {
        const bool asked = at + 1 < args.size();
        writeAll(asked ? environment.output : environment.error, listing(*commands, script));
        return asked ? 0 : UsageError;
    }
    const std::string& name = args[at + 1];
    const auto command = std::find_if(commands->begin(), commands->end(),
                                      [&](const Command& c) { return c.script == script && c.name == name; });
    if (command == commands->end()) {
        return client.fail("no command " + name + " in " + script + " (afterglow run " + script + " --help lists them)", UsageError);
    }
    // Stdin only when it is not a terminal: typed at a prompt, the command
    // would wait for its input until Ctrl+D. A stdin that is no terminal and
    // does not end (`ssh host cmd` with no -n) needs -n.
    return runCommand(client, *command, {args.begin() + std::ptrdiff_t(at) + 2, args.end()}, cwd, pane.value_or(client.pane()),
                      readInput && ::isatty(environment.input) != 1);
}

} // namespace

std::size_t inputPart(std::string_view data, bool eof)
{
    std::size_t size = 2; // the quotes
    std::size_t i = 0;
    while (i < data.size()) {
        const std::size_t start = i;
        char32_t cp = 0;
        // A sequence cut at the end of what came so far may complete later.
        if (!eof && data.size() - start < 4 && !nextUtf8(data, i, cp)) return 0;
        i = start;
        const std::size_t cost = Json::nextStringPiece(data, i);
        if (size + cost > InputPartBytes) return start;
        size += cost;
    }
    return eof ? data.size() : 0;
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
