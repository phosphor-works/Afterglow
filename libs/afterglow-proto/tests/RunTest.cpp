// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "FakeServer.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Json.h>
#include <AfterglowProto/Run.h>

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
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
    std::function<void()> prepare; ///< runs in the child before Run::main
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
            environment.cwd = "/";
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
    EXPECT_EQ(outcome.err, "afterglow run: Standard input is over 1 KiB (Scripts.commandStdinLimit)\n");
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
    for (const auto& args : {many, std::vector<std::string>{"tools", "greet", "a\xFF"},
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
    EXPECT_LT(outcome.ms, Run::ReplyTimeoutMs / 2);
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
    const Outcome outcome = client.wait();
    EXPECT_EQ(outcome.status, 1);
    EXPECT_TRUE(outcome.out.empty());
    // The list gets what the connection left of the second, not a second of its own.
    EXPECT_LT(outcome.ms, Run::CompleteTimeoutMs * 3 / 2);
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
    EXPECT_EQ(outcome.err, "afterglow run: bad ?[31mred?\n");
}

TEST(RunClient, AlignsTheListingByCharacters)
{
    FakeServer server;
    Client client(server, {});
    ASSERT_TRUE(list(server, {command("tools", "a", false, "First"), command("tools", "bb", false, "Second", "é")}));
    // "bb é" takes four columns in five bytes, and "a" has no usage to space.
    EXPECT_EQ(client.wait().out, "tools\n  a     First\n  bb é  Second\n");
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
    EXPECT_NE(outcome.err.find("lost afterglow: the server sent a line that is not a JSON object"), std::string::npos) << outcome.err;
}
