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
#include <csignal>
#include <cstring>
#include <optional>

#include <fcntl.h>
#include <unistd.h>

namespace AfterglowProto::Run {

namespace {

const char* const Usage = R"(usage: afterglow run                          list the commands of the script packages
       afterglow run ID --help                one package's commands
       afterglow run [-n] [--pane N] ID COMMAND [ARGS...]
                                              run a command, print its output and exit with its status

Runs a command of a script package in a running afterglow (Control.enabled and
Scripts.enabled in its config). The command acts on --pane N, else on the pane
that this shell runs in, else on the focused pane.

  -n, --no-stdin    do not pass standard input to the command
  --pane N          the pane that the command acts on

A command that reads standard input gets it only when standard input is not a
terminal. Exit status 2 is a usage error or an unknown command, 124 a command
that ran out of time (Scripts.commandTimeout), and 130 and 143 an interrupt.
)";

/// Text from a script, safe to print: no control character can reach the
/// terminal that shows it as an escape sequence.
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

bool writeAll(int fd, std::string_view data)
{
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data.remove_prefix(std::size_t(n));
    }
    return true;
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

    int fail(const std::string& message, int status = 1) const
    {
        writeAll(m_env.error, m_env.program + ": " + message + "\n");
        return status;
    }

    int usage(const std::string& message) const
    {
        writeAll(m_env.error, m_env.program + ": " + message + "\n\n" + Usage);
        return UsageError;
    }

    /// Connects and reads the commands. Reports its own errors unless @p quiet.
    std::optional<std::vector<Command>> commands(bool quiet)
    {
        if (m_socket.path.empty()) {
            if (!quiet) fail(m_socket.error);
            return std::nullopt;
        }
        IpcConnection::Error error;
        if (!m_ipc.connect(m_socket.path, &error)) {
            if (!quiet) {
                fail((m_socket.own ? "the afterglow that runs this pane has exited (" : "no running afterglow on this display (")
                     + error.message + ")");
            }
            return std::nullopt;
        }
        const auto list = m_ipc.call(Target, "list", {}, ReplyTimeoutMs, &error);
        if (!list) {
            if (!quiet) report(error);
            return std::nullopt;
        }
        std::vector<Command> out;
        for (const Json::Value& item : list->toArray()) {
            out.push_back({item["script"].toString(), item["command"].toString(), item["summary"].toString(),
                           item["usage"].toString(), item["stdin"].toBool()});
        }
        return out;
    }

    void report(const IpcConnection::Error& error) const
    {
        if (error.code == "NO_SUCH_TARGET") {
            fail("the running afterglow takes no commands (Control.enabled or Scripts.enabled is off)");
        } else if (error.timedOut) {
            fail("afterglow did not answer in " + std::to_string(ReplyTimeoutMs / 1000) + " seconds");
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
};

std::string listing(const std::vector<Command>& commands, std::string_view only)
{
    std::size_t width = 0;
    for (const Command& c : commands) {
        if (only.empty() || c.script == only) width = std::max(width, c.name.size() + 1 + c.usage.size());
    }
    std::string out;
    std::string script;
    for (const Command& c : commands) {
        if (!only.empty() && c.script != only) continue;
        if (c.script != script) {
            script = c.script;
            out += printable(script) + "\n";
        }
        std::string head = c.name + (c.usage.empty() ? "" : " " + c.usage);
        head.resize(std::max(head.size(), width), ' ');
        out += "  " + printable(head) + "  " + printable(c.summary) + "\n";
    }
    return out;
}

// The self-pipe of the signal handler: the run loop waits on it.
int g_wake[2] = {-1, -1};
volatile std::sig_atomic_t g_signal = 0;

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
        // No SA_RESTART: a read of stdin must return when a signal comes.
        ::sigaction(SIGINT, &action, &m_int);
        ::sigaction(SIGTERM, &action, &m_term);
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
    static int status() { return g_signal == SIGTERM ? Terminated : g_signal == SIGINT ? Interrupted : 0; }

private:
    struct sigaction m_int{}, m_term{}, m_pipe{};
    bool m_installed = false;
};

/// Sends stdin in parts until it ends or the terminal has enough.
/// Returns 0, or the exit status to stop with.
int sendInput(Client& client, const std::string& run)
{
    std::string buffer;
    bool eof = false;
    while (true) {
        while (!eof && buffer.size() < InputPartBytes * 2) {
            char chunk[65536];
            const ssize_t n = ::read(client.env().input, chunk, sizeof chunk);
            if (n < 0 && errno == EINTR) {
                if (Signals::status()) return Signals::status();
                continue;
            }
            if (n < 0) return client.fail(std::string("cannot read standard input: ") + std::strerror(errno));
            if (n == 0) eof = true;
            buffer.append(chunk, std::size_t(n));
        }
        if (Signals::status()) return Signals::status();
        const std::size_t size = inputPart(buffer, eof);
        const bool last = eof && size == buffer.size();
        IpcConnection::Error error;
        const auto more = client.ipc().call(Target, "input", {run, buffer.substr(0, size), last}, ReplyTimeoutMs, &error);
        if (!more) {
            client.report(error);
            return 1;
        }
        buffer.erase(0, size);
        // False: the terminal keeps no more (Scripts.commandStdinLimit).
        if (last || !more->toBool()) return 0;
    }
}

std::string workingDirectory(const Environment& env)
{
    if (!env.cwd.empty()) return env.cwd;
    std::string path(4096, '\0');
    while (!::getcwd(path.data(), path.size())) {
        if (errno != ERANGE || path.size() > (1u << 20)) return {}; // the terminal then uses the pane's
        path.resize(path.size() * 2);
    }
    path.resize(std::strlen(path.c_str()));
    return path;
}

int runCommand(Client& client, const Command& command, const std::vector<std::string>& args, std::int64_t pane,
               bool readInput)
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
    const auto id = client.ipc().send(Target, "run",
                                      {*run, command.script, command.name, std::move(arguments), workingDirectory(client.env()), pane},
                                      &error);
    if (!id) {
        client.report(error);
        return 1;
    }
    auto cancel = [&] {
        IpcConnection::Error ignored;
        client.ipc().send(Target, "cancel", {*run}, &ignored);
    };
    while (true) {
        Json::Value frame;
        const auto status = client.ipc().next(frame, -1, signals.fd());
        if (status == IpcConnection::Read::Woken) {
            cancel();
            return Signals::status();
        }
        if (status != IpcConnection::Read::Frame) return client.fail("lost afterglow: the connection closed");
        if (IpcConnection::answers(frame, *id)) {
            const auto result = IpcConnection::answer(frame, &error);
            if (!result) {
                if (error.code == NoSuchCommand) return client.fail(error.message, UsageError);
                client.report(error);
                return 1;
            }
            const auto exit = (*result)["exit"].toInteger();
            return exit && *exit >= 0 && *exit <= 255 ? int(*exit) : 1;
        }
        const std::string& type = frame["type"].toString();
        const std::string& signal = frame["signal"].toString();
        const Json::Array& eventArgs = frame["args"].toArray();
        if (type != "event" || frame["target"].toString() != Target || eventArgs.size() != 2
            || eventArgs[0].toString() != *run)
            continue;
        if (signal == "stdout" && !writeAll(client.env().output, eventArgs[1].toString())) {
            cancel();
            return BrokenPipe;
        }
        if (signal == "stderr") writeAll(client.env().error, eventArgs[1].toString());
    }
}

std::optional<std::int64_t> paneNumber(std::string_view text)
{
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value < 1 || value > 0xFFFFFFFFLL) return std::nullopt;
    return value;
}

} // namespace

std::size_t inputPart(std::string_view data, bool eof)
{
    std::size_t size = 2; // the quotes
    std::size_t i = 0;
    while (i < data.size()) {
        const std::size_t start = i;
        char32_t cp = 0;
        std::size_t cost;
        if (!nextUtf8(data, i, cp)) {
            // A sequence cut at the end of what came so far may complete later.
            if (!eof && data.size() - start < 4) return start > 0 && size > InputPartBytes / 2 ? start : 0;
            i = start + 1;
            cost = 3; // U+FFFD
        } else if (cp == '"' || cp == '\\' || cp == '\n' || cp == '\r' || cp == '\t') {
            cost = 2;
        } else {
            cost = cp < 0x20 ? 6 : i - start;
        }
        if (size + cost > InputPartBytes) return start;
        size += cost;
    }
    return eof ? data.size() : 0;
}

int main(const std::vector<std::string>& args, const Environment& environment)
{
    Client client(environment);
    if (!args.empty() && (args[0] == "-h" || args[0] == "--help")) {
        writeAll(environment.output, Usage);
        return 0;
    }
    if (!args.empty() && args[0] == "--complete") {
        // For shell completion: script IDs, or "COMMAND\tSUMMARY" of one. Quiet.
        if (args.size() > 2) return UsageError;
        const auto commands = client.commands(true);
        if (!commands) return 1;
        std::string out;
        std::string last;
        for (const Command& c : *commands) {
            if (args.size() == 1 && c.script != last) out += printable(c.script) + "\n";
            if (args.size() == 2 && c.script == args[1]) out += printable(c.name) + "\t" + printable(c.summary) + "\n";
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
    const auto commands = client.commands(false);
    if (!commands) return 1;
    if (at == args.size()) {
        if (at > 0) return client.usage("which package?");
        const std::string out = listing(*commands, {});
        writeAll(environment.output, out.empty() ? "No script package has commands.\n" : out);
        return 0;
    }
    const std::string& script = args[at];
    const bool known = std::any_of(commands->begin(), commands->end(), [&](const Command& c) { return c.script == script; });
    if (!known) return client.fail("no package " + printable(script) + " with commands (afterglow run lists them)", UsageError);
    if (at + 1 == args.size() || args[at + 1] == "--help" || args[at + 1] == "-h") {
        const bool asked = at + 1 < args.size();
        writeAll(asked ? environment.output : environment.error, listing(*commands, script));
        return asked ? 0 : UsageError;
    }
    const std::string& name = args[at + 1];
    const auto command = std::find_if(commands->begin(), commands->end(),
                                      [&](const Command& c) { return c.script == script && c.name == name; });
    if (command == commands->end()) {
        return client.fail("no command " + printable(name) + " in " + printable(script) + " (afterglow run "
                               + printable(script) + " --help lists them)",
                           UsageError);
    }
    // Stdin only when it is not a terminal: from cron or `ssh host cmd` it
    // could be an open descriptor that never ends.
    return runCommand(client, *command, {args.begin() + std::ptrdiff_t(at) + 2, args.end()}, pane.value_or(client.pane()),
                      readInput && ::isatty(environment.input) != 1);
}

} // namespace AfterglowProto::Run
