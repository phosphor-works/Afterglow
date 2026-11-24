// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The proxy of a daemon for a client on another host (`aglow-mux proxy`), and
// how a client reaches a daemon (MuxConnect): through a proxy command, and
// over the candidate sockets. The proxy runs in the environment of the test
// daemon, which names the test's own sockets; a socket of the test plays the
// daemon where the test needs one that does what it says. The proxy commands
// are scripts of the test. No test reaches the user's daemon, ssh or another
// host.

#include "TestDaemon.h"

#include "MuxConnect.h"

#include <spawn.h>

using namespace MuxTest;

namespace {

/// The environment of the clients of @p daemon, as NAME=VALUE entries.
Environment environmentOf(const TestDaemon& daemon)
{
    Environment environment;
    for (const auto& [name, value] : daemon.cleanEnvironment())
        environment.push_back(name + "=" + value);
    return environment;
}

/// A listening socket at @p path, in a directory of mode 0700 that it makes.
/// -1, with a failure of the test, when it cannot.
int listenAt(const std::string& path)
{
    const std::string directory = std::filesystem::path(path).parent_path().string();
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        ADD_FAILURE() << "cannot create " << directory << ": " << std::strerror(errno);
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof address.sun_path, "%s", path.c_str());
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(fd, 8) != 0) {
        ADD_FAILURE() << "cannot listen on " << path << ": " << std::strerror(errno);
        if (fd >= 0)
            ::close(fd);
        return -1;
    }
    return fd;
}

/// The next client of @p listener within five seconds, or -1.
int acceptOne(int listener)
{
    pollfd ready{listener, POLLIN, 0};
    if (::poll(&ready, 1, TestWaitMs) <= 0)
        return -1;
    return ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
}

/// The next line of @p fd, without its '\n', read byte by byte: the bytes
/// after it stay in the socket. Empty after five seconds or at the end.
std::string lineOf(int fd)
{
    std::string text;
    while (true) {
        pollfd ready{fd, POLLIN, 0};
        if (::poll(&ready, 1, TestWaitMs) <= 0)
            return {};
        char c = 0;
        if (::read(fd, &c, 1) != 1)
            return {};
        if (c == '\n')
            return text;
        text += c;
    }
}

/// Sends all of @p text on socket @p fd, without a SIGPIPE.
bool sendAll(int fd, std::string_view text)
{
    while (!text.empty()) {
        const ssize_t n = ::send(fd, text.data(), text.size(), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        text.remove_prefix(std::size_t(n));
    }
    return true;
}

/// Writes what the pipe @p fd, which does not block, takes of @p text, without
/// a SIGPIPE when its reader is gone: the bytes that went, or -1 with errno.
ssize_t writeSome(int fd, std::string_view text)
{
    sigset_t pipe;
    ::sigemptyset(&pipe);
    ::sigaddset(&pipe, SIGPIPE);
    sigset_t old;
    ::pthread_sigmask(SIG_BLOCK, &pipe, &old);
    const ssize_t n = ::write(fd, text.data(), text.size());
    const int saved = errno;
    const timespec now{0, 0};
    while (::sigtimedwait(&pipe, nullptr, &now) == SIGPIPE) { }
    ::pthread_sigmask(SIG_SETMASK, &old, nullptr);
    errno = saved;
    return n;
}

/// @p size bytes of a pattern that does not repeat in step with any buffer,
/// from byte @p from of it.
std::string pattern(std::uint64_t from, std::size_t size)
{
    std::string bytes(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = char('a' + (from + i) % 23);
    return bytes;
}

/// Reads @p size bytes of @p fd, within @p wait: fewer when the end or the
/// time came first.
std::string readBytes(int fd, std::size_t size, std::chrono::milliseconds wait)
{
    std::string bytes;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    char buffer[65536];
    while (bytes.size() < size) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        pollfd ready{fd, POLLIN, 0};
        if (left <= 0 || ::poll(&ready, 1, int(left)) <= 0)
            break;
        const ssize_t n = ::read(fd, buffer, std::min(sizeof buffer, size - bytes.size()));
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
            break;
        bytes.append(buffer, std::size_t(n));
    }
    return bytes;
}

/// All that @p fd sends until its end, within @p wait; nothing when the end
/// did not come.
std::optional<std::string> readToEnd(int fd, std::chrono::milliseconds wait)
{
    std::string bytes;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    char buffer[65536];
    while (true) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        pollfd ready{fd, POLLIN, 0};
        if (left <= 0 || ::poll(&ready, 1, int(left)) <= 0)
            return std::nullopt;
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n < 0)
            return std::nullopt;
        if (n == 0)
            return bytes;
        bytes.append(buffer, std::size_t(n));
    }
}

/// A socket at @p path that plays a daemon of this protocol for one client:
/// it answers the client's version call, and a proxy's callerEnvironment
/// call, and then leaves the connection to the test, which reads it when it
/// wants.
struct FakeDaemon
{
    explicit FakeDaemon(const std::string& path)
        : listener(listenAt(path))
    {
    }
    ~FakeDaemon()
    {
        if (client >= 0)
            ::close(client);
        if (listener >= 0)
            ::close(listener);
    }
    FakeDaemon(const FakeDaemon&) = delete;
    FakeDaemon& operator=(const FakeDaemon&) = delete;
    /// Takes the next client and answers its version call, and with
    /// @p proxy the callerEnvironment call that a proxy makes next.
    bool answer(bool proxy = true)
    {
        client = acceptOne(listener);
        if (client < 0)
            return false;
        const auto request = Json::parse(lineOf(client));
        if (!request || (*request)["fn"].toString() != Fn::Version)
            return false;
        if (!sendAll(client,
                     AfterglowProto::ipcReply((*request)["id"].toInteger().value_or(0),
                                              Json::Object{{"protocol", ProtocolVersion}})))
            return false;
        if (!proxy)
            return true;
        const auto sent = Json::parse(lineOf(client));
        if (!sent || (*sent)["fn"].toString() != Fn::CallerEnvironment)
            return false;
        environment = environmentFrom((*sent)["args"].toArray().at(1), nullptr).value_or(environment);
        return sendAll(client, AfterglowProto::ipcReply((*sent)["id"].toInteger().value_or(0), nullptr));
    }
    void closeClient()
    {
        if (client >= 0)
            ::close(client);
        client = -1;
    }
    int listener = -1;
    int client = -1;
    /// What the proxy's callerEnvironment call sent.
    std::vector<std::pair<std::string, std::string>> environment;
};

/// `aglow-mux proxy` in the environment of @p daemon: it finds the sockets of
/// that daemon, or a FakeDaemon at them, never the user's. The test holds its
/// standard input and output, which do not block. Its standard error goes to
/// proxy.err in the test's directory.
struct ProxyRun
{
    explicit ProxyRun(const TestDaemon& daemon)
        : errorFile(daemon.dir() + "/proxy.err")
    {
        int in[2] = {-1, -1};
        int out[2] = {-1, -1};
        if (::pipe2(in, O_CLOEXEC) != 0 || ::pipe2(out, O_CLOEXEC) != 0) {
            ADD_FAILURE() << "no pipes for the proxy: " << std::strerror(errno);
            for (const int fd : {in[0], in[1], out[0], out[1]}) {
                if (fd >= 0)
                    ::close(fd);
            }
            return;
        }
        Environment store = environmentOf(daemon);
        std::vector<char*> envp;
        for (std::string& entry : store)
            envp.push_back(entry.data());
        envp.push_back(nullptr);
        std::array<char*, 3> argv{const_cast<char*>(AFTERGLOW_MUX), const_cast<char*>(ProxyArgument), nullptr};
        posix_spawn_file_actions_t actions;
        int spawned = ::posix_spawn_file_actions_init(&actions);
        if (spawned == 0) {
            if (::posix_spawn_file_actions_adddup2(&actions, in[0], 0) != 0
                || ::posix_spawn_file_actions_adddup2(&actions, out[1], 1) != 0
                || ::posix_spawn_file_actions_addopen(&actions, 2, errorFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                                      0600)
                    != 0) {
                spawned = EINVAL;
            }
            if (spawned == 0)
                spawned = ::posix_spawn(&pid, AFTERGLOW_MUX, &actions, nullptr, argv.data(), envp.data());
            ::posix_spawn_file_actions_destroy(&actions);
        }
        ::close(in[0]);
        ::close(out[1]);
        input = in[1];
        output = out[0];
        if (spawned != 0) {
            pid = -1;
            ADD_FAILURE() << "cannot run aglow-mux proxy: " << std::strerror(spawned);
            return;
        }
        for (const int fd : {input, output})
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    ~ProxyRun()
    {
        closeInput();
        closeOutput();
        if (pid > 0 && !status) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
    }
    ProxyRun(const ProxyRun&) = delete;
    ProxyRun& operator=(const ProxyRun&) = delete;
    void closeInput()
    {
        if (input >= 0)
            ::close(input);
        input = -1;
    }
    void closeOutput()
    {
        if (output >= 0)
            ::close(output);
        output = -1;
    }
    /// The exit status, 128 + the signal for one that a signal ended, once
    /// the proxy ended within @p wait; else nothing, and it still runs.
    std::optional<int> exitStatus(std::chrono::milliseconds wait = TestWait)
    {
        if (status || pid <= 0)
            return status;
        const auto deadline = std::chrono::steady_clock::now() + wait;
        while (true) {
            int raw = 0;
            const pid_t reaped = ::waitpid(pid, &raw, WNOHANG);
            if (reaped == pid) {
                status = WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
                return status;
            }
            if (reaped < 0 || std::chrono::steady_clock::now() >= deadline)
                return std::nullopt;
            std::this_thread::sleep_for(20ms);
        }
    }
    std::string errors() const
    {
        return readFile(errorFile);
    }

    std::string errorFile;
    pid_t pid = -1;
    int input = -1;
    int output = -1;
    std::optional<int> status;
};

/// A route through @p proxy, a script of the test, to host "h".
Route routeThrough(const std::string& proxy)
{
    Route route;
    route.proxy = {proxy};
    route.host = "h";
    return route;
}

/// The process ID that a proxy script wrote to @p file, 0 without one.
pid_t pidIn(const std::string& file)
{
    return pid_t(std::strtol(readFile(file).c_str(), nullptr, 10));
}

} // namespace

// The proxy holds at most a megabyte for a daemon that does not read: the
// client's writes then wait, and every byte reaches the daemon, in order,
// once it reads.
TEST(MuxProxy, AProxyHoldsLittleForADaemonThatDoesNotRead)
{
    TestDaemon daemon;
    FakeDaemon fake(daemon.socket());
    ASSERT_GE(fake.listener, 0);
    ProxyRun proxy(daemon);
    ASSERT_GT(proxy.pid, 0);
    ASSERT_TRUE(fake.answer()) << proxy.errors();
    std::uint64_t written = 0;
    while (written < (std::uint64_t(32) << 20)) {
        const ssize_t n = writeSome(proxy.input, pattern(written, 65536));
        if (n > 0) {
            written += std::uint64_t(n);
            continue;
        }
        if (n < 0 && errno != EAGAIN) {
            ADD_FAILURE() << "the proxy took no more: " << std::strerror(errno) << " " << proxy.errors();
            break;
        }
        // A second without room: the proxy reads no more.
        pollfd room{proxy.input, POLLOUT, 0};
        if (::poll(&room, 1, 1000) == 0)
            break;
    }
    // Its own megabyte, and the buffers of the pipe and the socket.
    EXPECT_GT(written, std::uint64_t(1) << 20);
    EXPECT_LT(written, std::uint64_t(8) << 20);
    const std::string received = readBytes(fake.client, std::size_t(written), 10s);
    EXPECT_EQ(received.size(), written);
    EXPECT_TRUE(received == pattern(0, std::size_t(written)));
}

// The end of the client's side waits for the bytes before it: they reach the
// daemon, and then the end. The proxy goes on until the daemon closes.
TEST(MuxProxy, WhatWaitsGoesWhenStandardInputEnds)
{
    TestDaemon daemon;
    FakeDaemon fake(daemon.socket());
    ASSERT_GE(fake.listener, 0);
    ProxyRun proxy(daemon);
    ASSERT_GT(proxy.pid, 0);
    ASSERT_TRUE(fake.answer()) << proxy.errors();
    // Less than the proxy holds: the daemon reads none of it yet.
    const std::string sent = pattern(0, std::size_t(512) << 10);
    std::size_t at = 0;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (at < sent.size() && std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = writeSome(proxy.input, std::string_view(sent).substr(at));
        if (n > 0) {
            at += std::size_t(n);
            continue;
        }
        if (n < 0 && errno != EAGAIN)
            break;
        pollfd room{proxy.input, POLLOUT, 0};
        ::poll(&room, 1, 100);
    }
    ASSERT_EQ(at, sent.size()) << proxy.errors();
    proxy.closeInput();
    EXPECT_TRUE(readBytes(fake.client, sent.size(), 10s) == sent);
    const auto rest = readToEnd(fake.client, 5s);
    ASSERT_TRUE(rest) << "the end of the client's side did not come";
    EXPECT_EQ(*rest, "");
    EXPECT_FALSE(proxy.exitStatus(200ms).has_value());
    fake.closeClient();
    EXPECT_EQ(proxy.exitStatus(), 0) << proxy.errors();
}

// Whether the proxy relays: a line of the client reaches the daemon. Until
// then the proxy checks its answer, and bytes of the daemon that came with
// that answer are an error (aglow-mux proxy refuses them).
bool relays(ProxyRun& proxy, FakeDaemon& fake)
{
    const std::string line = "relay\n";
    // Standard input is a pipe: write(2), as send(2) takes only sockets.
    return writeSome(proxy.input, line) == ssize_t(line.size()) && readBytes(fake.client, line.size(), 5s) == line;
}

// The daemon's close ends the proxy after the daemon's last bytes, while the
// client's side is still open.
TEST(MuxProxy, TheDaemonsCloseEndsTheProxyAfterItsLastBytes)
{
    TestDaemon daemon;
    FakeDaemon fake(daemon.socket());
    ASSERT_GE(fake.listener, 0);
    ProxyRun proxy(daemon);
    ASSERT_GT(proxy.pid, 0);
    ASSERT_TRUE(fake.answer()) << proxy.errors();
    ASSERT_TRUE(relays(proxy, fake)) << proxy.errors();
    ASSERT_TRUE(sendAll(fake.client, "last\n"));
    fake.closeClient();
    EXPECT_EQ(readToEnd(proxy.output, 5s), std::optional<std::string>("last\n"));
    EXPECT_EQ(proxy.exitStatus(), 0) << proxy.errors();
}

// The proxy sends its own environment, that of the user's ssh login, before
// it carries the client: the daemon reads no other process's.
TEST(MuxProxy, TheProxySendsItsEnvironmentFirst)
{
    TestDaemon daemon;
    FakeDaemon fake(daemon.socket());
    ASSERT_GE(fake.listener, 0);
    ProxyRun proxy(daemon);
    ASSERT_GT(proxy.pid, 0);
    ASSERT_TRUE(fake.answer()) << proxy.errors();
    auto sent = fake.environment;
    auto expected = daemon.cleanEnvironment();
    std::sort(sent.begin(), sent.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_TRUE(sent == expected);
    ASSERT_TRUE(relays(proxy, fake)) << proxy.errors();
}

// A client that no longer reads ends the proxy at the daemon's next bytes.
TEST(MuxProxy, AClientThatStopsReadingEndsTheProxy)
{
    TestDaemon daemon;
    FakeDaemon fake(daemon.socket());
    ASSERT_GE(fake.listener, 0);
    ProxyRun proxy(daemon);
    ASSERT_GT(proxy.pid, 0);
    ASSERT_TRUE(fake.answer()) << proxy.errors();
    ASSERT_TRUE(relays(proxy, fake)) << proxy.errors();
    proxy.closeOutput();
    ASSERT_TRUE(sendAll(fake.client, std::string(4096, 'x')));
    EXPECT_EQ(proxy.exitStatus(), 0) << proxy.errors();
}

// A proxy command that fails says why on its standard error, which is the
// message (ssh that cannot reach the host, for one).
TEST(MuxConnect, TheErrorOutputOfAProxyIsTheMessage)
{
    TestDaemon daemon;
    const TempDir temp;
    const std::string proxy =
        temp.script("ssh", "echo 'ssh: connect to host h port 22: Connection refused' >&2; exit 255");
    if (proxy.empty())
        GTEST_SKIP() << "the temporary directory runs no program";
    std::string error;
    const Reached reached = reach(routeThrough(proxy), false, CallTimeoutMs, environmentOf(daemon), &error);
    EXPECT_FALSE(reached.connection);
    EXPECT_EQ(error, "ssh: connect to host h port 22: Connection refused");
}

// A proxy command that does not answer within the wait of its first answer
// (Mux.signInTimeout) is no daemon, and ends: the end of its connection
// first, then SIGTERM, then a kill when it still does not end.
TEST(MuxConnect, AProxyThatDoesNotAnswerEndsAfterItsWait)
{
    TestDaemon daemon;
    const TempDir temp;
    for (const bool ignoresTerm : {false, true}) {
        SCOPED_TRACE(ignoresTerm ? "it ignores SIGTERM" : "SIGTERM ends it");
        const std::string pidFile = (temp.path / (ignoresTerm ? "ignores" : "ends")).string();
        const std::string proxy = temp.script(ignoresTerm ? "ssh-ignores" : "ssh-ends",
                                              std::string(ignoresTerm ? "trap '' TERM; " : "") + "echo $$ > '" + pidFile
                                                  + "'; exec sleep 30");
        if (proxy.empty())
            GTEST_SKIP() << "the temporary directory runs no program";
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        const Reached reached = reach(routeThrough(proxy), false, 300, environmentOf(daemon), &error);
        const auto took = std::chrono::steady_clock::now() - start;
        EXPECT_FALSE(reached.connection);
        EXPECT_FALSE(error.empty());
        // The wait of the call, not CallTimeoutMs, and then the end of the
        // proxy: 1 s for its own end, 1 s more after SIGTERM.
        EXPECT_GE(took, 300ms);
        EXPECT_LT(took, ignoresTerm ? 6s : 4s);
        const pid_t pid = pidIn(pidFile);
        ASSERT_GT(pid, 0);
        // Reaped: no process has that ID now.
        EXPECT_NE(::kill(pid, 0), 0);
    }
}

// The first answer of a proxy may take longer than a call
// (CallTimeoutMs): ssh can ask for a password first.
TEST(MuxConnect, AProxyMayTakeItsSignInWaitForTheFirstAnswer)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const TempDir temp;
    // The proxy of this host, after a sign-in longer than a call.
    const std::string proxy = temp.script("ssh", std::string("sleep 6; exec '") + AFTERGLOW_MUX + "' \"$@\"");
    if (proxy.empty())
        GTEST_SKIP() << "the temporary directory runs no program";
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    const Reached reached = reach(routeThrough(proxy), false, 15000, environmentOf(daemon), &error);
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(CallTimeoutMs));
    ASSERT_TRUE(reached.connection) << error;
    EXPECT_EQ(error, "");
    EXPECT_TRUE(reached.proxy);
}

// A socket whose daemon closes the connection without an answer counts as
// no daemon, and so does nothing: the next candidate answers. A candidate
// that cannot be used says why, unless another one answers.
TEST(MuxConnect, TheNextSocketAnswersWhenTheFirstFails)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string closing = daemon.dir() + "/closes/socket";
    const int listener = listenAt(closing);
    ASSERT_GE(listener, 0);
    // The daemon of the first socket ends: it reads the call and closes.
    const auto closeNext = [listener] {
        return std::thread([listener] {
            const int fd = acceptOne(listener);
            if (fd < 0)
                return;
            (void)lineOf(fd);
            ::close(fd);
        });
    };
    AfterglowProto::MuxSockets sockets;
    sockets.candidates.push_back({closing, daemon.dir() + "/closes"});
    {
        // Alone, it is no daemon, and no error.
        std::thread closer = closeNext();
        std::string error;
        const Reached reached = connectAny(sockets, &error);
        closer.join();
        EXPECT_FALSE(reached.connection);
        EXPECT_EQ(error, "");
    }
    sockets.candidates.push_back(daemon.sockets().candidates.front());
    {
        std::thread closer = closeNext();
        std::string error;
        const Reached reached = connectAny(sockets, &error);
        closer.join();
        EXPECT_TRUE(reached.connection) << error;
        EXPECT_EQ(error, "");
    }
    ::close(listener);
    // A socket that this user may not connect to: an error alone, and none
    // once the next one answers. Root connects to any.
    if (::geteuid() == 0)
        return;
    const std::string denied = daemon.dir() + "/denied/socket";
    const int locked = listenAt(denied);
    ASSERT_GE(locked, 0);
    ASSERT_EQ(::chmod(denied.c_str(), 0), 0);
    sockets.candidates.front() = {denied, daemon.dir() + "/denied"};
    AfterglowProto::MuxSockets alone;
    alone.candidates.push_back(sockets.candidates.front());
    std::string error;
    EXPECT_FALSE(connectAny(alone, &error).connection);
    EXPECT_FALSE(error.empty());
    const Reached reached = connectAny(sockets, &error);
    EXPECT_TRUE(reached.connection) << error;
    EXPECT_EQ(error, "");
    ::close(locked);
}
