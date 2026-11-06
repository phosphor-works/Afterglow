// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "FakeServer.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Json.h>
#include <AfterglowProto/Run.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace AfterglowProto;
using AfterglowProto::Test::FakeServer;

namespace {

/// An entry of the terminal's list.
Json::Value command(std::string script, std::string name, bool input = false, std::string summary = "Summary",
                    std::string usage = "")
{
    return Json::Object{{"script", std::move(script)}, {"command", std::move(name)}, {"summary", std::move(summary)},
                        {"usage", std::move(usage)},   {"palette", ""},            {"stdin", input}};
}

struct Outcome
{
    int status = -1;
    int signal = -1;
    std::string out;
    std::string err;
    long ms = 0;
};

struct Options
{
    bool input = true; ///< a pipe for stdin, else /dev/null
    std::function<void()> prepare = {}; ///< runs in the child before Run::main
    std::string cwd = "/"; ///< Environment::cwd: empty for the working directory
    std::string program = "aglow run";
};

/// Run::main in a child process of its own, with pipes for its standard
/// descriptors: signals, closed descriptors and their dispositions stay out
/// of the test process.
class Client
{
public:
    Client(const FakeServer& server, std::vector<std::string> args, Options options = {})
        : m_start(std::chrono::steady_clock::now())
    {
        int in[2] = {-1, -1}, out[2], err[2], result[2];
        if ((options.input && ::pipe(in) != 0) || ::pipe(out) != 0 || ::pipe(err) != 0 || ::pipe(result) != 0) return;
        const std::string path = server.path();
        m_pid = ::fork();
        if (m_pid == 0) {
            const int input = options.input ? in[0] : ::open("/dev/null", O_RDONLY);
            ::dup2(input, 0);
            ::dup2(out[1], 1);
            ::dup2(err[1], 2);
            for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1], result[0], input}) {
                if (fd > 2) ::close(fd);
            }
            if (options.prepare) options.prepare();
            Run::Environment environment;
            environment.env = [path](const char* name) -> const char* {
                return std::string_view(name) == "AFTERGLOW_SOCKET" ? path.c_str() : nullptr;
            };
            environment.cwd = options.cwd;
            environment.program = options.program;
            int signal = 0;
            const int status = Run::main(args, environment, &signal);
            const int answer[2] = {status, signal};
            [[maybe_unused]] const ssize_t written = ::write(result[1], answer, sizeof answer);
            ::_exit(0);
        }
        for (int fd : {in[0], out[1], err[1], result[1]}) {
            if (fd >= 0) ::close(fd);
        }
        m_in = in[1];
        m_out = out[0];
        m_err = err[0];
        m_result = result[0];
    }
    ~Client()
    {
        if (m_pid > 0) {
            ::kill(m_pid, SIGKILL);
            ::waitpid(m_pid, nullptr, 0);
        }
        for (int fd : {m_in, m_out, m_err, m_result}) {
            if (fd >= 0) ::close(fd);
        }
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool started() const { return m_pid > 0; }
    pid_t pid() const { return m_pid; }
    bool write(std::string_view data)
    {
        while (!data.empty()) {
            const ssize_t n = ::write(m_in, data.data(), data.size());
            if (n <= 0) return false;
            data.remove_prefix(std::size_t(n));
        }
        return true;
    }
    void closeInput() { closeFd(m_in); }
    /// Leaves the child's stdout without a reader.
    void closeOutput() { closeFd(m_out); }
    /// Whether the pipe of the child's stdout, which the test does not read
    /// until wait(), became full in @p timeoutMs. A write(2) of more than it
    /// holds then sleeps in the kernel until a reader or a signal comes.
    bool outputFull(int timeoutMs = 5000) const
    {
        const int capacity = ::fcntl(m_out, F_GETPIPE_SZ);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            int queued = 0;
            if (capacity <= 0 || ::ioctl(m_out, FIONREAD, &queued) != 0) return false;
            if (queued >= capacity) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    /// The child's output until it exits, and its status. A child that takes
    /// longer than @p timeoutMs is killed: status -1.
    Outcome wait(int timeoutMs = 10000)
    {
        Outcome outcome;
        std::string result;
        const auto deadline = m_start + std::chrono::milliseconds(timeoutMs);
        while (m_out >= 0 || m_err >= 0 || m_result >= 0) {
            pollfd fds[3] = {{m_out, POLLIN, 0}, {m_err, POLLIN, 0}, {m_result, POLLIN, 0}};
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0 || ::poll(fds, 3, int(left.count())) <= 0) break;
            std::string* into[3] = {&outcome.out, &outcome.err, &result};
            int* fd[3] = {&m_out, &m_err, &m_result};
            for (int i = 0; i < 3; ++i) {
                if (!fds[i].revents) continue;
                char chunk[65536];
                const ssize_t n = ::read(*fd[i], chunk, sizeof chunk);
                if (n <= 0) {
                    closeFd(*fd[i]);
                } else {
                    into[i]->append(chunk, std::size_t(n));
                }
            }
        }
        if (result.size() == 2 * sizeof(int)) {
            std::memcpy(&outcome.status, result.data(), sizeof(int));
            std::memcpy(&outcome.signal, result.data() + sizeof(int), sizeof(int));
        }
        if (m_pid > 0) {
            if (outcome.status < 0) ::kill(m_pid, SIGKILL);
            ::waitpid(m_pid, nullptr, 0);
            m_pid = -1;
        }
        outcome.ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count());
        return outcome;
    }

private:
    static void closeFd(int& fd)
    {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }

    std::chrono::steady_clock::time_point m_start;
    pid_t m_pid = -1;
    int m_in = -1, m_out = -1, m_err = -1, m_result = -1;
};

/// Accepts the client and answers its list with @p commands.
bool list(FakeServer& server, Json::Array commands)
{
    if (!server.accept()) return false;
    const auto call = server.call();
    return call && (*call)["fn"].toString() == "list" && server.reply(*call, std::move(commands));
}

/// The client's next call, which must be @p fn.
std::optional<Json::Value> expect(FakeServer& server, std::string_view fn)
{
    auto call = server.call();
    if (!call || (*call)["fn"].toString() != fn) return std::nullopt;
    return call;
}

std::string runId(const Json::Value& call)
{
    return call["args"].toArray().empty() ? std::string() : call["args"].toArray()[0].toString();
}

} // namespace

TEST(RunInput, PartsStayUnderTheLimitAndKeepCharactersWhole)
{
    // Control characters cost six bytes each once written.
    const std::string controls(Run::InputPartBytes, '\x01');
    const std::size_t part = Run::inputPart(controls, false);
    EXPECT_EQ(part, (Run::InputPartBytes - 2) / 6);
    EXPECT_LE(Json::write(Json::Value(controls.substr(0, part))).size(), Run::InputPartBytes);

    // A four-byte character across the limit goes whole into the next part.
    std::string text(Run::InputPartBytes - 4, 'a');
    text += "😀";
    text += std::string(100, 'b');
    const std::size_t first = Run::inputPart(text, false);
    EXPECT_EQ(first, Run::InputPartBytes - 4);
    EXPECT_EQ(Run::inputPart(std::string_view(text).substr(first), true), text.size() - first);
}

TEST(RunInput, APartIsFullWhenTheNextCharacterWouldNotFit)
{
    // Exactly the limit: one more byte may still come, and fit or not.
    EXPECT_EQ(Run::inputPart(std::string(Run::InputPartBytes - 2, 'a'), false), 0u);
    EXPECT_EQ(Run::inputPart(std::string(Run::InputPartBytes - 2, 'a'), true), Run::InputPartBytes - 2);
    EXPECT_EQ(Run::inputPart(std::string(Run::InputPartBytes - 1, 'a'), false), Run::InputPartBytes - 2);
    // A limit of its own: the quotes and six bytes.
    EXPECT_EQ(Run::inputPart("abcdefgh", false, 8), 6u);
    EXPECT_EQ(Run::inputPart("abcdef", false, 8), 0u);
    EXPECT_EQ(Run::inputPart("abcdef", true, 8), 6u);
    EXPECT_EQ(Run::inputPart("ab\x01", true, 8), 2u); // \u0001 takes six
    // A smaller limit counts as 8, so that each piece fits in a part.
    EXPECT_EQ(Run::inputPart("\x01\x01", true, 1), 1u);
    EXPECT_EQ(Run::inputPart("\x01", true, 0), 1u);
}

TEST(RunInput, AGrowingBufferGivesThePartsOfOneScan)
{
    std::string text;
    for (int i = 0; text.size() < 2 * Run::InputPartBytes + 1000; ++i) text += i % 7 ? "word \xc3\xa9 " : "\x01\xf0\x9f\x98\x80\n";
    // Reads of a pipe: pieces that cut characters.
    Run::InputParts parts;
    std::string buffer;
    std::size_t fed = 0;
    std::size_t sent = 0;
    std::size_t count = 0;
    bool eof = false;
    while (!eof || !buffer.empty()) {
        if (!eof) {
            const std::size_t n = std::min<std::size_t>(65521, text.size() - fed);
            buffer.append(text, fed, n);
            fed += n;
            eof = fed == text.size();
        }
        const std::size_t size = parts.next(buffer, eof);
        ASSERT_EQ(size, Run::inputPart(buffer, eof));
        if (size == 0) continue;
        EXPECT_LE(Json::write(Json::Value(buffer.substr(0, size))).size(), Run::InputPartBytes);
        EXPECT_TRUE(text.compare(sent, size, buffer, 0, size) == 0);
        sent += size;
        ++count;
        buffer.erase(0, size);
        parts.restart();
    }
    EXPECT_EQ(sent, text.size());
    EXPECT_GT(count, 2u);
    EXPECT_TRUE(parts.text());
}

TEST(RunInput, TellsInputThatIsNotUtf8)
{
    Run::InputParts parts;
    EXPECT_EQ(parts.next("ab\xf0\x9f", false), 0u);
    EXPECT_TRUE(parts.text()); // the sequence can still complete
    EXPECT_EQ(parts.next("ab\xf0\x9f\x98\x80", false), 0u);
    EXPECT_TRUE(parts.text());
    EXPECT_EQ(parts.next("ab\xf0\x9f\x98\x80\xff" "cdef", false), 0u);
    EXPECT_FALSE(parts.text());
    // At the end of the input, a cut sequence is not UTF-8.
    Run::InputParts cut;
    EXPECT_EQ(cut.next("ab\xf0\x9f", true), 4u);
    EXPECT_FALSE(cut.text());
    // Before the end, bytes at the end of the data that no later byte can
    // complete: a byte that starts no sequence, a continuation byte alone,
    // an overlong form, a surrogate and a code point past U+10FFFF.
    for (const char* tail : {"\xff", "\x80", "\xc0", "\xe0\x80", "\xed\xa0", "\xf0\x80", "\xf4\x90", "\xf5", "\xe2\x41"}) {
        Run::InputParts parts;
        parts.next(std::string("ab") + tail, false);
        EXPECT_FALSE(parts.text()) << tail;
    }
    // The starts of a character wait for the rest of it.
    for (const char* tail : {"\xc3", "\xe0\xa0", "\xed\x9f", "\xf0\x90\x80", "\xf4\x8f\xbf"}) {
        Run::InputParts parts;
        EXPECT_EQ(parts.next(std::string("ab") + tail, false), 0u) << tail;
        EXPECT_TRUE(parts.text()) << tail;
    }
}

TEST(RunInput, WaitsForMoreUntilTheEnd)
{
    EXPECT_EQ(Run::inputPart("short", false), 0u);
    EXPECT_EQ(Run::inputPart("short", true), 5u);
    EXPECT_EQ(Run::inputPart("", true), 0u);
    // A sequence cut at the end waits for its other bytes, unless input
    // ended: then each byte goes as U+FFFD.
    EXPECT_EQ(Run::inputPart("ab\xF0\x9F", true), 4u);
    const auto parsed = Json::parse(Json::write(Json::Value(std::string("ab\xF0\x9F"))));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->toString(), "ab\xEF\xBF\xBD\xEF\xBF\xBD");

    // A cut sequence just below the limit: the part waits for its last
    // byte, then goes without the character, which does not fit.
    std::string text(Run::InputPartBytes - 3, 'a');
    text += "\xF0\x9F\x98";
    EXPECT_EQ(Run::inputPart(text, false), 0u);
    text += "\x80";
    EXPECT_EQ(Run::inputPart(text, false), Run::InputPartBytes - 3);
}

TEST(RunClient, PassesOutputAndTheExitStatusThrough)
{
    FakeServer server;
    Client client(server, {"tools", "greet", "you"});
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(list(server, {command("tools", "greet")}));
    const auto run = expect(server, "run");
    ASSERT_TRUE(run);
    const Json::Array& args = (*run)["args"].toArray();
    ASSERT_EQ(args.size(), 6u);
    EXPECT_EQ(args[1].toString(), "tools");
    EXPECT_EQ(args[2].toString(), "greet");
    EXPECT_EQ(args[3], Json::Value(Json::Array{"you"}));
    EXPECT_EQ(args[4].toString(), "/");
    EXPECT_EQ(args[5].toInteger(), 0);
    server.event("commands", "stdout", {runId(*run), "hello\n"});
    server.event("commands", "stderr", {runId(*run), "note\n"});
    server.reply(*run, Json::Object{{"exit", 7}});
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, 7);
    EXPECT_EQ(outcome.signal, 0);
    EXPECT_EQ(outcome.out, "hello\n");
    EXPECT_EQ(outcome.err, "note\n");
}

TEST(RunClient, ASignalCancelsTheRunAndIsReported)
{
    for (const auto& [number, status] : {std::pair{SIGINT, 130}, std::pair{SIGTERM, 143}}) {
        FakeServer server;
        Client client(server, {"tools", "wait"});
        ASSERT_TRUE(client.started());
        ASSERT_TRUE(list(server, {command("tools", "wait")}));
        const auto run = expect(server, "run");
        ASSERT_TRUE(run);
        ::kill(client.pid(), number);
        const auto cancel = expect(server, "cancel");
        ASSERT_TRUE(cancel);
        EXPECT_EQ(runId(*cancel), runId(*run));
        const Outcome outcome = client.wait();
        // The status is for a caller that cannot end by the signal.
        EXPECT_EQ(outcome.status, status);
        EXPECT_EQ(outcome.signal, number);
    }
}

TEST(RunClient, AClosedStdoutEndsTheRunAsSigpipeDoes)
{
    for (const bool ignored : {false, true}) {
        FakeServer server;
        Client client(server, {"tools", "talk"}, {.prepare = [ignored] {
                                                      if (ignored) ::signal(SIGPIPE, SIG_IGN);
                                                  }});
        ASSERT_TRUE(client.started());
        client.closeOutput();
        ASSERT_TRUE(list(server, {command("tools", "talk")}));
        const auto run = expect(server, "run");
        ASSERT_TRUE(run);
        server.event("commands", "stdout", {runId(*run), "lost\n"});
        ASSERT_TRUE(expect(server, "cancel"));
        const Outcome outcome = client.wait();
        EXPECT_EQ(outcome.status, Run::BrokenPipe);
        // A process that ignores SIGPIPE ends by its status only.
        EXPECT_EQ(outcome.signal, ignored ? 0 : SIGPIPE);
    }
}

TEST(RunClient, ASignalEndsAWriteThatAFullStdoutHoldsBack)
{
    FakeServer server;
    Client client(server, {"tools", "talk"});
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(list(server, {command("tools", "talk")}));
    const auto run = expect(server, "run");
    ASSERT_TRUE(run);
    // More than the pipe holds: once it is full, the client waits for room,
    // and only the signal can end that.
    const std::string text(1024 * 1024, 'x');
    ASSERT_TRUE(server.event("commands", "stdout", {runId(*run), text}));
    ASSERT_TRUE(client.outputFull());
    // It waits on a description of its own, with O_NONBLOCK, so that no
    // blocking write is left for the signal to miss. Descriptor 1, which the
    // shell shares, stays as it was.
    const auto flags = [&](const std::string& fd) {
        std::ifstream info("/proc/" + std::to_string(client.pid()) + "/fdinfo/" + fd);
        std::string key;
        std::string value;
        while (info >> key >> value) {
            if (key == "flags:") return std::stol(value, nullptr, 8);
        }
        return -1L;
    };
    const auto target = [&](const std::string& fd) {
        return std::filesystem::read_symlink("/proc/" + std::to_string(client.pid()) + "/fd/" + fd).string();
    };
    EXPECT_EQ(flags("1") & O_NONBLOCK, 0);
    bool own = false;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/" + std::to_string(client.pid()) + "/fd")) {
        const std::string fd = entry.path().filename().string();
        if (fd != "1" && target(fd) == target("1") && (flags(fd) & O_NONBLOCK)) own = true;
    }
    EXPECT_TRUE(own);
    ::kill(client.pid(), SIGTERM);
    const auto cancel = expect(server, "cancel");
    ASSERT_TRUE(cancel);
    EXPECT_EQ(runId(*cancel), runId(*run));
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, Run::Terminated);
    EXPECT_EQ(outcome.signal, SIGTERM);
    EXPECT_LT(outcome.out.size(), text.size());
}

TEST(RunClient, WaitsForRoomInAStdoutThatDoesNotBlock)
{
    FakeServer server;
    Client client(server, {"tools", "talk"}, {.prepare = [] { ::fcntl(1, F_SETFL, ::fcntl(1, F_GETFL) | O_NONBLOCK); }});
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(list(server, {command("tools", "talk")}));
    const auto run = expect(server, "run");
    ASSERT_TRUE(run);
    // More than a pipe holds: the client's write fills it and must wait.
    const std::string text(100 * 1024, 'x');
    server.event("commands", "stdout", {runId(*run), text});
    server.reply(*run, Json::Object{{"exit", 0}});
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, 0) << outcome.err;
    EXPECT_EQ(outcome.out.size(), text.size());
}

TEST(RunClient, InputOverTheLimitIsAUsageError)
{
    FakeServer server;
    Client client(server, {"tools", "read"});
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.write("abc"));
    client.closeInput();
    ASSERT_TRUE(list(server, {command("tools", "read", true)}));
    const auto input = expect(server, "input");
    ASSERT_TRUE(input);
    server.error(*input, Run::InputTooLarge, "Standard input is over 1 KiB (Scripts.commandStdinLimit)");
    // No run comes: the connection closes.
    EXPECT_FALSE(server.call());
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, Run::UsageError);
    EXPECT_EQ(outcome.err, "aglow run: Standard input is over 1 KiB (Scripts.commandStdinLimit)\n");
}

TEST(RunClient, InputThatIsNotUtf8IsAUsageError)
{
    // A byte that is not UTF-8 is refused before the input ends, also as the
    // last byte so far, and a sequence that the end cuts when it ends.
    for (const auto& [input, ends] : {std::pair{std::string("abc\xff" "def"), false}, std::pair{std::string("abc\xff"), false},
                                      std::pair{std::string("ab\xf0\x9f"), true}}) {
        FakeServer server;
        Client client(server, {"tools", "read"});
        ASSERT_TRUE(client.started());
        ASSERT_TRUE(client.write(input));
        if (ends) client.closeInput();
        ASSERT_TRUE(list(server, {command("tools", "read", true)}));
        // Neither input nor a run comes: the connection closes.
        EXPECT_FALSE(server.call());
        const Outcome outcome = client.wait();
        EXPECT_EQ(outcome.status, Run::UsageError);
        EXPECT_EQ(outcome.err, "aglow run: standard input is not UTF-8 text\n");
    }
}

TEST(RunClient, ALostTerminalEndsTheWaitForInput)
{
    FakeServer server;
    Client client(server, {"tools", "read"});
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(list(server, {command("tools", "read", true)}));
    // Stdin stays open, and nothing comes.
    server.hangUp();
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, 1);
    EXPECT_EQ(outcome.err, "aglow run: lost Afterglow: the server closed the connection\n");
}

TEST(RunClient, SendsAPartOfInputAsSoonAsItIsFull)
{
    FakeServer server;
    Client client(server, {"tools", "read"});
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(list(server, {command("tools", "read", true)}));
    // One part and a little more, and stdin stays open. The client reads it
    // only after the list: a pipe holds less.
    ASSERT_TRUE(client.write(std::string(Run::InputPartBytes + 8 * 1024, 'x')));
    auto input = expect(server, "input");
    ASSERT_TRUE(input);
    EXPECT_EQ((*input)["args"].toArray()[1].toString().size(), Run::InputPartBytes - 2);
    EXPECT_FALSE((*input)["args"].toArray()[2].toBool(true));
    server.reply(*input, true);
    client.closeInput();
    input = expect(server, "input");
    ASSERT_TRUE(input);
    EXPECT_EQ((*input)["args"].toArray()[1].toString().size(), 8u * 1024 + 2);
    EXPECT_TRUE((*input)["args"].toArray()[2].toBool());
    server.reply(*input, true);
    const auto run = expect(server, "run");
    ASSERT_TRUE(run);
    server.reply(*run, Json::Object{{"exit", 0}});
    EXPECT_EQ(client.wait().status, 0);
}

TEST(RunClient, AClosedStdinIsEmptyInput)
{
    FakeServer server;
    // Without its descriptor 0, the socket would take that number.
    Client client(server, {"tools", "read"}, {.input = false, .prepare = [] { ::close(0); }});
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(list(server, {command("tools", "read", true)}));
    const auto input = expect(server, "input");
    ASSERT_TRUE(input);
    EXPECT_EQ((*input)["args"], Json::Value(Json::Array{runId(*input), "", true}));
    server.reply(*input, true);
    const auto run = expect(server, "run");
    ASSERT_TRUE(run);
    server.reply(*run, Json::Object{{"exit", 0}});
    EXPECT_EQ(client.wait().status, 0);
}

TEST(RunClient, RefusesWhatTheTerminalWouldBeforeItConnects)
{
    std::vector<std::string> many{"tools", "greet"};
    many.resize(2 + Run::MaxArgs + 1, "x");
    // Arguments within their limits that make a run call over 1 MiB.
    std::vector<std::string> large{"tools", "greet"};
    std::string wide;
    for (int i = 0; i < 4000; ++i) wide += "\xc3\xa9";
    large.resize(2 + Run::MaxArgs, wide);
    for (const auto& args : {many, large, std::vector<std::string>{"tools", "greet", "a\xFF"},
                             std::vector<std::string>{"tools", "greet", std::string(Run::MaxArgLength + 1, 'x')},
                             std::vector<std::string>{"-n"}, std::vector<std::string>{"--pane", "0", "tools", "greet"}}) {
        FakeServer server;
        Client client(server, args);
        ASSERT_TRUE(client.started());
        const Outcome outcome = client.wait();
        EXPECT_EQ(outcome.status, Run::UsageError) << outcome.err;
        EXPECT_FALSE(outcome.err.empty());
        EXPECT_FALSE(server.connectionWaits()) << outcome.err;
    }
}

TEST(RunClient, SendsTheWorkingDirectoryOrNoneForADeletedOne)
{
    const auto cwdOfRun = [](Options options) {
        FakeServer server;
        Client client(server, {"tools", "greet"}, std::move(options));
        if (!list(server, {command("tools", "greet")})) return std::string("no list");
        const auto run = expect(server, "run");
        if (!run) return std::string("no run");
        server.reply(*run, Json::Object{{"exit", 0}});
        client.wait();
        return (*run)["args"].toArray()[4].toString();
    };
    EXPECT_EQ(cwdOfRun({.prepare = [] { [[maybe_unused]] const int ignored = ::chdir("/"); }, .cwd = ""}), "/");
    // A deleted directory has no path: the run gets the pane's.
    EXPECT_EQ(cwdOfRun({.prepare =
                            [] {
                                char dir[] = "/tmp/afterglow-run-cwd-XXXXXX";
                                if (::mkdtemp(dir) && ::chdir(dir) == 0) ::rmdir(dir);
                            },
                        .cwd = ""}),
              "");
}

TEST(RunClient, RefusesAWorkingDirectoryThatIsNotUtf8)
{
    FakeServer server;
    Client client(server, {"tools", "greet"}, {.cwd = "/tmp/a\xff"});
    ASSERT_TRUE(client.started());
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, 1);
    EXPECT_EQ(outcome.err, "aglow run: the working directory is not UTF-8 text\n");
    EXPECT_FALSE(server.connectionWaits());
}

TEST(RunClient, AStatusThatNoProcessHasIsOne)
{
    for (const auto& [exit, status] : {std::pair{Json::Value(255), 255}, std::pair{Json::Value(256), 1}, std::pair{Json::Value(-1), 1},
                                       std::pair{Json::Value(1.5), 1}, std::pair{Json::Value("0"), 1}}) {
        FakeServer server;
        Client client(server, {"tools", "greet"});
        ASSERT_TRUE(list(server, {command("tools", "greet")}));
        const auto run = expect(server, "run");
        ASSERT_TRUE(run);
        server.reply(*run, Json::Object{{"exit", exit}});
        EXPECT_EQ(client.wait().status, status) << Json::write(exit);
    }
}

TEST(RunClient, AcknowledgesTheOutputThatItWrote)
{
    FakeServer server;
    // Stdout goes to /dev/null: the test reads none of it until the run ends.
    Client client(server, {"tools", "talk"}, {.prepare = [] {
                                                  const int null = ::open("/dev/null", O_WRONLY);
                                                  ::dup2(null, 1);
                                                  ::close(null);
                                              }});
    ASSERT_TRUE(list(server, {command("tools", "talk")}));
    const auto run = expect(server, "run");
    ASSERT_TRUE(run);
    const std::string id = runId(*run);
    const std::string text(100 * 1024, 'x');
    // An event line as the client counts it: '\n' included, as FakeServer writes it.
    const std::size_t line =
        Json::write(Json::Object{{"type", "event"}, {"target", "commands"}, {"signal", "stdout"}, {"args", Json::Array{id, text}}}).size() + 1;
    // The ack comes after the event that makes AckBytes, and not before.
    const std::size_t events = Run::AckBytes / line + 1;
    for (std::size_t i = 0; i < events; ++i) ASSERT_TRUE(server.event("commands", "stdout", {id, text}));
    const auto ack = expect(server, "ack");
    ASSERT_TRUE(ack);
    EXPECT_EQ((*ack)["args"], Json::Value(Json::Array{id, std::int64_t(events * line)}));
    server.reply(*run, Json::Object{{"exit", 0}});
    EXPECT_EQ(client.wait().status, 0);
}

TEST(RunClient, NamesTheProgramThatRunsIt)
{
    FakeServer server;
    // Help is an option, also after others.
    for (const auto& args : {std::vector<std::string>{"--help"}, std::vector<std::string>{"-n", "--pane", "3", "-h"}}) {
        Client client(server, args, {.program = "afterglow run"});
        const Outcome outcome = client.wait();
        EXPECT_EQ(outcome.status, 0);
        EXPECT_EQ(outcome.out.rfind("Usage: afterglow run  ", 0), 0u) << outcome.out;
        EXPECT_NE(outcome.out.find("\nafterglow run ends by that signal"), std::string::npos) << outcome.out;
        EXPECT_FALSE(server.connectionWaits());
    }
    Client client(server, {"--pane"}, {.program = "afterglow run"});
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, Run::UsageError);
    EXPECT_EQ(outcome.err.rfind("afterglow run: --pane needs a pane ID\n\nUsage: afterglow run", 0), 0u) << outcome.err;
}

TEST(RunClient, TheTerminalSaysWhyACommandIsNotInTheList)
{
    {
        // The list leaves out a disabled script: its run goes, and the
        // refusal says why.
        FakeServer server;
        Client client(server, {"tools", "greet"});
        ASSERT_TRUE(list(server, {command("other", "go")}));
        const auto run = expect(server, "run");
        ASSERT_TRUE(run);
        EXPECT_EQ((*run)["args"].toArray()[1].toString(), "tools");
        server.error(*run, "CALL_FAILED", "Script tools is disabled. afterglow ctl scripts shows why");
        const Outcome outcome = client.wait();
        EXPECT_EQ(outcome.status, 1);
        EXPECT_EQ(outcome.err, "aglow run: Script tools is disabled. afterglow ctl scripts shows why\n");
    }
    // A command that the terminal does not know either is a usage error,
    // with where to look. Its run gets no input.
    for (const auto& [args, message] :
         {std::pair{std::vector<std::string>{"tools", "nope"}, std::string("no command nope in tools (aglow run tools --help lists them)")},
          std::pair{std::vector<std::string>{"gone", "nope"}, std::string("no script gone with commands (aglow run lists them)")}}) {
        FakeServer server;
        Client client(server, args);
        ASSERT_TRUE(client.write("abc"));
        client.closeInput();
        ASSERT_TRUE(list(server, {command("tools", "greet", true)}));
        const auto run = expect(server, "run");
        ASSERT_TRUE(run);
        server.error(*run, Run::NoSuchCommand, "No command nope in tools");
        const Outcome outcome = client.wait();
        EXPECT_EQ(outcome.status, Run::UsageError);
        EXPECT_EQ(outcome.err, "aglow run: " + message + "\n");
    }
}

TEST(RunClient, AScriptWithoutACommandSaysWhichItHas)
{
    FakeServer server;
    Client client(server, {"tools"});
    ASSERT_TRUE(list(server, {command("tools", "a", false, "First")}));
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, Run::UsageError);
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_EQ(outcome.err, "aglow run: which command of tools?\ntools\n  a  First\n");
}

TEST(RunClient, CompletesOnlyNamesThatAShellTakesAsTheyAre)
{
    const Json::Array commands{command("tools", "go", false, "Go\tnow"), command("tools", "x;y"), command("$(boom)", "go"),
                               command("a b", "go"), command("more", "it")};
    FakeServer server;
    {
        Client client(server, {"--complete"});
        ASSERT_TRUE(list(server, commands));
        EXPECT_EQ(client.wait().out, "tools\nmore\n");
    }
    {
        Client client(server, {"--complete", "tools"});
        ASSERT_TRUE(list(server, commands));
        EXPECT_EQ(client.wait().out, "go\tGo?now\n");
    }
    {
        // Every command of every script, in the order of the list.
        Client client(server, {"--complete", "--all"});
        ASSERT_TRUE(list(server, commands));
        EXPECT_EQ(client.wait().out, "tools\tgo\tGo?now\nmore\tit\tSummary\n");
    }
}

TEST(RunClient, CompletionDoesNotWaitLong)
{
    FakeServer server;
    Client client(server, {"--complete"});
    ASSERT_TRUE(server.accept());
    ASSERT_TRUE(expect(server, "list")); // and no answer
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, 1);
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_LT(outcome.ms, Run::CompleteTimeoutMs * 3 / 2);
}

TEST(RunClient, CompletionWaitsOnceForTheConnectionAndTheList)
{
    // A backlog of 0 holds one connection: the client's connect waits
    // behind it until the server takes that one.
    FakeServer server(0);
    ASSERT_TRUE(server.valid());
    IpcConnection first;
    IpcConnection::Error error;
    ASSERT_TRUE(first.connect(server.path(), &error, 1000)) << error.message;
    Client client(server, {"--complete"});
    std::this_thread::sleep_for(std::chrono::milliseconds(Run::CompleteTimeoutMs * 7 / 10));
    ASSERT_TRUE(server.accept()); // the first
    ASSERT_TRUE(server.accept()); // the client's, late
    ASSERT_TRUE(expect(server, "list")); // and no answer
    const auto asked = std::chrono::steady_clock::now();
    const Outcome outcome = client.wait();
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - asked);
    EXPECT_EQ(outcome.status, 1);
    EXPECT_TRUE(outcome.out.empty());
    // The list gets what the connection left of the second, about 0.3 s,
    // not a second of its own.
    EXPECT_LT(waited.count(), Run::CompleteTimeoutMs * 2 / 3);
}

TEST(RunClient, PrintsTheTerminalsTextWithoutControlCharacters)
{
    FakeServer server;
    Client client(server, {"tools", "greet"});
    ASSERT_TRUE(list(server, {command("tools", "greet")}));
    const auto run = expect(server, "run");
    ASSERT_TRUE(run);
    server.error(*run, "CALL_FAILED", "bad \x1b[31mred\x07");
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, 1);
    EXPECT_EQ(outcome.err, "aglow run: bad ?[31mred?\n");
}

TEST(RunClient, AlignsTheListingByCharacters)
{
    FakeServer server;
    Client client(server, {});
    ASSERT_TRUE(list(server, {command("tools", "a", false, "First"), command("tools", "bb", false, "Second", "é"),
                              command("tools", "cc", false, "Third", "一")}));
    // "bb é" takes four columns in five bytes, "cc 一" five in six, and "a"
    // has no usage to space.
    EXPECT_EQ(client.wait().out, "tools\n  a      First\n  bb é   Second\n  cc 一  Third\n");
}

TEST(RunClient, SaysWhyTheConnectionWasLost)
{
    FakeServer server;
    Client client(server, {"tools", "greet"});
    ASSERT_TRUE(list(server, {command("tools", "greet")}));
    ASSERT_TRUE(expect(server, "run"));
    server.write("garbage\n");
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, 1);
    EXPECT_NE(outcome.err.find("lost Afterglow: the server sent a line that is not a JSON object"), std::string::npos) << outcome.err;
}
