// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowPty/Pty.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace AfterglowPty;
using namespace std::chrono_literals;

namespace {

// What the child wrote to the terminal and to its stdout pipe, apart.
struct Listener : IPtyListener
{
    std::mutex mutex;
    std::condition_variable changed;
    std::string terminal;
    std::string output;
    bool closed = false;
    int status = -1;
    bool outputAfterClose = false;
    /// A slow reader: the child refills the pipe between two reads.
    std::chrono::microseconds outputDelay{0};

    void ptyData(std::string_view bytes) override
    {
        std::lock_guard lock(mutex);
        terminal.append(bytes);
        changed.notify_all();
    }
    void ptyOutput(std::string_view bytes) override
    {
        if (outputDelay.count() > 0) std::this_thread::sleep_for(outputDelay);
        std::lock_guard lock(mutex);
        outputAfterClose |= closed;
        output.append(bytes);
        changed.notify_all();
    }
    void ptyClosed(int exitStatus) override
    {
        std::lock_guard lock(mutex);
        closed = true;
        status = exitStatus;
        changed.notify_all();
    }
    void ptyWritable() override
    {
        std::lock_guard lock(mutex);
        ++writable;
        changed.notify_all();
    }
    int writable = 0;
    bool waitClosed()
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 10s, [this] { return closed; });
    }
};

/// A variable of the test process, set for one test and put back after it,
/// also when an assertion ends the test early.
class Variable
{
public:
    Variable(const char* name, const char* value)
        : m_name(name)
    {
        if (const char* old = std::getenv(name)) m_old = old;
        ::setenv(name, value, 1);
    }
    ~Variable()
    {
        if (m_old) ::setenv(m_name, m_old->c_str(), 1);
        else ::unsetenv(m_name);
    }
    Variable(const Variable&) = delete;
    Variable& operator=(const Variable&) = delete;

private:
    const char* m_name;
    std::optional<std::string> m_old;
};

SpawnOptions shell(const std::string& script)
{
    SpawnOptions options;
    options.argv = {"/bin/sh", "-c", script};
    return options;
}

int openDescriptors()
{
    int count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) ++count;
    return count;
}

std::string numberedLines(std::size_t atLeast)
{
    std::string text;
    for (int i = 0; text.size() < atLeast; ++i) text += "line " + std::to_string(i) + '\n';
    return text;
}

} // namespace

TEST(Pty, PipedInputAndOutputStayApartFromTheTerminal)
{
    // fzf's way: the list on stdin, the interface on /dev/tty, the answer on stdout.
    SpawnOptions options = shell("read line; printf 'ui:%s\\n' \"$line\" > /dev/tty; printf 'answer:%s' \"$line\"");
    options.input = std::string("picked\nnot read\n");
    options.captureOutput = true;
    Listener listener;
    std::string error;
    auto pty = Pty::spawn(options, listener, &error);
    ASSERT_TRUE(pty) << error;
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_EQ(listener.output, "answer:picked");
    EXPECT_NE(listener.terminal.find("ui:picked"), std::string::npos) << listener.terminal;
    EXPECT_EQ(listener.terminal.find("answer:"), std::string::npos) << listener.terminal;
    EXPECT_EQ(listener.terminal.find("not read"), std::string::npos) << "the piped input is not echoed";
    EXPECT_FALSE(listener.outputAfterClose);
    ASSERT_TRUE(WIFEXITED(listener.status));
    EXPECT_EQ(WEXITSTATUS(listener.status), 0);
}

TEST(Pty, TheTerminalStaysTheControllingTerminalAndStandardError)
{
    SpawnOptions options = shell("[ -t 0 ] && echo in-tty >&2; [ -t 1 ] && echo out-tty >&2; [ -t 2 ] && echo err-tty >&2;"
                                 " exec 3</dev/tty && echo has-tty >&2");
    options.input = std::string();
    options.captureOutput = true;
    Listener listener;
    auto pty = Pty::spawn(options, listener);
    ASSERT_TRUE(pty);
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_EQ(listener.terminal.find("in-tty"), std::string::npos) << listener.terminal;
    EXPECT_EQ(listener.terminal.find("out-tty"), std::string::npos) << listener.terminal;
    EXPECT_NE(listener.terminal.find("err-tty"), std::string::npos) << listener.terminal;
    EXPECT_NE(listener.terminal.find("has-tty"), std::string::npos) << listener.terminal;
    EXPECT_TRUE(listener.output.empty());
}

TEST(Pty, WithoutInheritanceTheChildGetsOnlyTheGivenEnvironment)
{
    const Variable parent("PTY_TEST_PARENT", "1");
    for (const bool inherit : {true, false}) {
        SCOPED_TRACE(inherit ? "inherited" : "not inherited");
        SpawnOptions options = shell("echo \"[$PTY_TEST_PARENT][$GIVEN]\"; env | wc -l");
        const char* path = std::getenv("PATH");
        options.environment = {{"GIVEN", "yes"}, {"PATH", path ? path : "/usr/bin:/bin"}};
        options.inheritEnvironment = inherit;
        options.captureOutput = true;
        Listener listener;
        auto pty = Pty::spawn(options, listener);
        // Not ASSERT: the other row still runs.
        EXPECT_TRUE(pty);
        if (!pty || !listener.waitClosed()) continue;
        std::lock_guard lock(listener.mutex);
        const auto lineEnd = listener.output.find('\n');
        EXPECT_EQ(listener.output.substr(0, lineEnd), inherit ? "[1][yes]" : "[][yes]");
        if (!inherit) {
            // GIVEN and PATH, and what sh sets itself (PWD, SHLVL, _).
            int count = -1;
            const std::string rest = lineEnd == std::string::npos ? std::string() : listener.output.substr(lineEnd + 1);
            const auto digits = std::min(rest.find_first_not_of(' '), rest.size());
            const auto parsed = std::from_chars(rest.data() + digits, rest.data() + rest.size(), count);
            EXPECT_EQ(parsed.ec, std::errc()) << listener.output;
            EXPECT_LE(count, 5) << listener.output;
        }
    }
}

TEST(Pty, WithoutInheritanceOrPathTheProgramIsFoundInTheSystemDirectories)
{
    // No PATH at all: the search takes /bin:/usr/bin, as execvp's does.
    SpawnOptions options;
    options.argv = {"sh", "-c", "printf found"};
    options.inheritEnvironment = false;
    options.captureOutput = true;
    Listener listener;
    std::string error;
    auto pty = Pty::spawn(options, listener, &error);
    ASSERT_TRUE(pty) << error;
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_EQ(listener.output, "found");
}

TEST(Pty, WithoutInheritanceTheDefaultShellIsNotTheParentsShell)
{
    // The mux daemon's SHELL is not the session's: the account's shell is.
    const Variable parent("SHELL", "/nonexistent/parent-shell");
    SpawnOptions options;
    EXPECT_EQ(Pty::defaultShell(options), "/nonexistent/parent-shell");
    options.inheritEnvironment = false;
    EXPECT_NE(Pty::defaultShell(options), "/nonexistent/parent-shell");
    options.environment = {{"SHELL", "/nonexistent/given-shell"}};
    EXPECT_EQ(Pty::defaultShell(options), "/nonexistent/given-shell");
}

TEST(Pty, AProgramWithAnEmptyNameIsNotFound)
{
    SpawnOptions options;
    options.argv = {""};
    Listener listener;
    std::string error;
    EXPECT_EQ(Pty::spawn(options, listener, &error), nullptr);
    EXPECT_NE(error.find(std::strerror(ENOENT)), std::string::npos) << error;
}

TEST(Pty, ASetOrUnsetReplacesEveryEarlierOneOfTheName)
{
    SpawnOptions options;
    options.environment = {{"KEPT", "1"}, {"NAME", "a"}, {"NAME", "b"}};
    options.unsetEnvironment = {"NAME", "GONE"};
    setVariable(options, "NAME", "c");
    EXPECT_EQ(options.environment, (std::vector<std::pair<std::string, std::string>>{{"KEPT", "1"}, {"NAME", "c"}}));
    EXPECT_EQ(options.unsetEnvironment, std::vector<std::string>{"GONE"});
    unsetVariable(options, "NAME");
    unsetVariable(options, "GONE");
    EXPECT_EQ(options.environment, (std::vector<std::pair<std::string, std::string>>{{"KEPT", "1"}}));
    EXPECT_EQ(options.unsetEnvironment, (std::vector<std::string>{"NAME", "GONE"}));
}

TEST(Pty, TheChildGetsTheGivenMask)
{
    // The parent's own mask stays as it was.
    const mode_t parent = ::umask(022);
    for (const int mask : {-1, 077}) {
        SCOPED_TRACE(mask);
        SpawnOptions options = shell("umask");
        options.umask = mask;
        options.captureOutput = true;
        Listener listener;
        auto pty = Pty::spawn(options, listener);
        EXPECT_TRUE(pty);
        if (!pty || !listener.waitClosed()) continue;
        std::lock_guard lock(listener.mutex);
        EXPECT_EQ(listener.output, mask < 0 ? "0022\n" : "0077\n");
    }
    EXPECT_EQ(::umask(parent), mode_t(022));
}

TEST(Pty, TheChildGetsTheGivenSoftFileLimit)
{
    rlimit parent{};
    ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &parent), 0);
    // A limit under the parent's soft one, so that the test runs under any
    // hard limit; the hard limit stays.
    const rlim_t given = std::min<rlim_t>(parent.rlim_cur, 4096) / 2;
    ASSERT_GT(given, 3u);
    for (const long limit : {-1L, long(given)}) {
        SCOPED_TRACE(limit);
        SpawnOptions options = shell("echo \"$(ulimit -Sn) $(ulimit -Hn)\"");
        options.fileLimit = limit;
        options.captureOutput = true;
        Listener listener;
        auto pty = Pty::spawn(options, listener);
        EXPECT_TRUE(pty);
        if (!pty || !listener.waitClosed()) continue;
        const auto shown = [](rlim_t value) { return value == RLIM_INFINITY ? std::string("unlimited") : std::to_string(value); };
        std::lock_guard lock(listener.mutex);
        EXPECT_EQ(listener.output, shown(limit < 0 ? parent.rlim_cur : given) + ' ' + shown(parent.rlim_max) + '\n');
    }
    rlimit after{};
    ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &after), 0);
    EXPECT_EQ(after.rlim_cur, parent.rlim_cur);
}

TEST(Pty, TheLastValueOfANameWinsAndASetWinsOverAnUnset)
{
    const Variable parent("PTY_TEST_PARENT", "parent");
    SpawnOptions options = shell("echo \"[$PTY_TEST_PARENT][$TWICE][$GONE]\"");
    options.environment = {{"TWICE", "first"}, {"PTY_TEST_PARENT", "given"}, {"TWICE", "second"}};
    options.unsetEnvironment = {"PTY_TEST_PARENT", "GONE"};
    options.captureOutput = true;
    Listener listener;
    auto pty = Pty::spawn(options, listener);
    ASSERT_TRUE(pty);
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_EQ(listener.output, "[given][second][]\n");
}

TEST(Pty, WithoutPipesTheStandardStreamsAreTheTerminal)
{
    Listener listener;
    auto pty = Pty::spawn(shell("[ -t 0 ] && [ -t 1 ] && echo all-tty"), listener);
    ASSERT_TRUE(pty);
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_NE(listener.terminal.find("all-tty"), std::string::npos) << listener.terminal;
    EXPECT_TRUE(listener.output.empty());
}

TEST(Pty, LargeInputAndOutputPassThroughThePipes)
{
    // More than a pipe holds, both ways at once: the I/O thread feeds and
    // reads without blocking on either.
    std::string input;
    for (int i = 0; i < 200000; ++i) input += "line " + std::to_string(i) + '\n';
    SpawnOptions options = shell("cat");
    options.input = input;
    options.captureOutput = true;
    Listener listener;
    auto pty = Pty::spawn(options, listener);
    ASSERT_TRUE(pty);
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_EQ(listener.output.size(), input.size());
    EXPECT_TRUE(listener.output == input);
}

TEST(Pty, AChildThatClosesItsInputEarlyDoesNotStopThePty)
{
    // The rest of the input meets a closed pipe (EPIPE): no SIGPIPE ends
    // the test process, and the exit status still arrives.
    SpawnOptions options = shell("exec 0<&-; sleep 0.2; printf done");
    options.input = std::string(4 * 1024 * 1024, 'x');
    options.captureOutput = true;
    Listener listener;
    auto pty = Pty::spawn(options, listener);
    ASSERT_TRUE(pty);
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_EQ(listener.output, "done");
    ASSERT_TRUE(WIFEXITED(listener.status));
    EXPECT_EQ(WEXITSTATUS(listener.status), 0);
}

// A writer that the full queue refused hears when there is room again, and
// only then: the mux daemon holds the input of a window until it fits.
TEST(Pty, AWriteThatTheFullQueueRefusedHearsOfRoom)
{
    // The child reads nothing for a while, then everything.
    Listener listener;
    auto pty = Pty::spawn(shell("stty raw -echo; sleep 1; exec cat > /dev/null"), listener);
    ASSERT_TRUE(pty);
    const std::string part(64 * 1024, 'x');
    ASSERT_TRUE(pty->write(part));
    std::this_thread::sleep_for(100ms);
    {
        std::lock_guard lock(listener.mutex);
        EXPECT_EQ(listener.writable, 0) << "room without a refusal";
    }
    bool refused = false;
    for (std::size_t sent = 0; sent <= 2 * Pty::MaxPendingInput && !refused; sent += part.size()) refused = !pty->write(part);
    ASSERT_TRUE(refused);
    std::unique_lock lock(listener.mutex);
    ASSERT_TRUE(listener.changed.wait_for(lock, 20s, [&] { return listener.writable > 0; }));
    lock.unlock();
    EXPECT_LE(pty->pendingInput(), Pty::MaxPendingInput / 2);
    EXPECT_TRUE(pty->write(part));
}

// A writer that waits for room hears too when the terminal takes no more
// input while the child runs on: it waits for nothing then.
TEST(Pty, AWriterThatWaitsHearsThatTheTerminalTakesNoMore)
{
    Listener listener;
    // The child reads nothing, closes its terminal, and lives on.
    auto pty = Pty::spawn(shell("stty raw -echo; sleep 1; exec sleep 10 0<&- 1>&- 2>&-"), listener);
    ASSERT_TRUE(pty);
    const std::string part(64 * 1024, 'x');
    bool refused = false;
    for (std::size_t sent = 0; sent <= 2 * Pty::MaxPendingInput && !refused; sent += part.size()) refused = !pty->write(part);
    ASSERT_TRUE(refused);
    EXPECT_TRUE(pty->accepting());
    std::unique_lock lock(listener.mutex);
    ASSERT_TRUE(listener.changed.wait_for(lock, 20s, [&] { return listener.writable > 0; }));
    EXPECT_FALSE(listener.closed) << "the child still runs";
    lock.unlock();
    EXPECT_FALSE(pty->accepting());
}

TEST(Pty, TheExitStatusComesAfterTheOutput)
{
    SpawnOptions options = shell("printf partial; exit 3");
    options.captureOutput = true;
    Listener listener;
    auto pty = Pty::spawn(options, listener);
    ASSERT_TRUE(pty);
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_EQ(listener.output, "partial");
    ASSERT_TRUE(WIFEXITED(listener.status));
    EXPECT_EQ(WEXITSTATUS(listener.status), 3);
}

TEST(Pty, AFastProducerIsReadToItsEnd)
{
    // Every read of a batch finds the pipe refilled. A used-up batch is not
    // the pipe's end: closing the pipe there cut the output, and the child
    // died of SIGPIPE.
    constexpr std::size_t Size = 8u * 1024 * 1024;
    SpawnOptions options = shell("head -c " + std::to_string(Size) + " /dev/zero");
    options.captureOutput = true;
    Listener listener;
    listener.outputDelay = 500us;
    std::string error;
    auto pty = Pty::spawn(std::move(options), listener, &error);
    ASSERT_TRUE(pty) << error;
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_EQ(listener.output.size(), Size);
    EXPECT_TRUE(std::all_of(listener.output.begin(), listener.output.end(), [](char c) { return c == '\0'; }));
    ASSERT_TRUE(WIFEXITED(listener.status)) << listener.status;
    EXPECT_EQ(WEXITSTATUS(listener.status), 0);
}

TEST(Pty, ThePipesOutliveTheTerminalWhileTheChildRuns)
{
    // Standard input and output are the pipes and standard error goes
    // elsewhere, so no fd of the child holds the terminal: the master hangs
    // up while the child still reads its input and writes its output.
    const std::string input = numberedLines(1024 * 1024);
    SpawnOptions options = shell("exec 2>/dev/null; cat");
    options.input = input;
    options.captureOutput = true;
    Listener listener;
    std::string error;
    auto pty = Pty::spawn(std::move(options), listener, &error);
    ASSERT_TRUE(pty) << error;
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_EQ(listener.output.size(), input.size());
    EXPECT_TRUE(listener.output == input);
    ASSERT_TRUE(WIFEXITED(listener.status)) << listener.status;
    EXPECT_EQ(WEXITSTATUS(listener.status), 0);
}

TEST(Pty, ADeferredStartFeedsAndReadsThePipes)
{
    SpawnOptions options = shell("cat");
    options.deferStart = true;
    options.input = std::string("deferred");
    options.captureOutput = true;
    Listener listener;
    std::string error;
    auto pty = Pty::spawn(std::move(options), listener, &error);
    ASSERT_TRUE(pty) << error;
    {
        // No callback before start(): a reader that ran before it would have
        // fed cat its input and read the echo by the end of this wait.
        std::unique_lock lock(listener.mutex);
        EXPECT_FALSE(listener.changed.wait_for(lock, 300ms, [&listener] { return !listener.output.empty() || listener.closed; }));
    }
    ASSERT_TRUE(pty->start(&error)) << error;
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_EQ(listener.output, "deferred");
    ASSERT_TRUE(WIFEXITED(listener.status)) << listener.status;
    EXPECT_EQ(WEXITSTATUS(listener.status), 0);
}

TEST(Pty, AFailedStartWithPipesReportsWhyAndLeavesNoDescriptor)
{
    const int before = openDescriptors();
    {
        SpawnOptions options;
        options.argv = {"/nonexistent/afterglow-pty-test"};
        options.input = std::string(1024 * 1024, 'x');
        options.captureOutput = true;
        Listener listener;
        std::string error;
        EXPECT_EQ(Pty::spawn(std::move(options), listener, &error), nullptr);
        EXPECT_NE(error.find("cannot run /nonexistent/afterglow-pty-test"), std::string::npos) << error;
    }
    {
        SpawnOptions options = shell("true");
        options.workingDirectory = "/nonexistent/afterglow-pty-test";
        options.input = std::string("x");
        options.captureOutput = true;
        Listener listener;
        std::string error;
        EXPECT_EQ(Pty::spawn(std::move(options), listener, &error), nullptr);
        EXPECT_NE(error.find("cannot enter directory /nonexistent/afterglow-pty-test"), std::string::npos) << error;
    }
    EXPECT_EQ(openDescriptors(), before);
}

TEST(Pty, PipedInputWithoutCaptureKeepsTheTerminalAsOutput)
{
    SpawnOptions options = shell("[ -t 0 ] || echo in-pipe; [ -t 1 ] && echo out-tty; read line; echo \"got:$line\"");
    options.input = std::string("from-pipe\n");
    Listener listener;
    std::string error;
    auto pty = Pty::spawn(std::move(options), listener, &error);
    ASSERT_TRUE(pty) << error;
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_NE(listener.terminal.find("in-pipe"), std::string::npos) << listener.terminal;
    EXPECT_NE(listener.terminal.find("out-tty"), std::string::npos) << listener.terminal;
    EXPECT_NE(listener.terminal.find("got:from-pipe"), std::string::npos) << listener.terminal;
    EXPECT_TRUE(listener.output.empty());
    ASSERT_TRUE(WIFEXITED(listener.status)) << listener.status;
    EXPECT_EQ(WEXITSTATUS(listener.status), 0);
}

TEST(Pty, TheChildGetsOnlyTheStandardStreams)
{
    // An fd of the parent without close-on-exec, which only the child's own
    // close keeps out, and the pipes' far ends, which it moved onto 0 and 1.
    struct Descriptor
    {
        int fd;
        ~Descriptor() { if (fd >= 0) ::close(fd); }
    } inherited{::open("/dev/null", O_RDONLY)};
    ASSERT_GE(inherited.fd, 3);
    const std::string probe = "leaks=; n=3; while [ $n -lt 256 ]; do [ -e /proc/self/fd/$n ] && leaks=\"$leaks $n\";"
                              " n=$((n+1)); done; echo \"leaks:[$leaks]\" >&2";
    for (const bool pipes : {false, true}) {
        SCOPED_TRACE(pipes ? "with pipes" : "plain");
        SpawnOptions options = shell(probe);
        if (pipes) {
            options.input = std::string("x");
            options.captureOutput = true;
        }
        Listener listener;
        std::string error;
        auto pty = Pty::spawn(std::move(options), listener, &error);
        // Not ASSERT: the other row still runs.
        EXPECT_TRUE(pty) << error;
        if (!pty) continue;
        const bool closed = listener.waitClosed();
        EXPECT_TRUE(closed);
        if (!closed) continue;
        std::lock_guard lock(listener.mutex);
        EXPECT_NE(listener.terminal.find("leaks:[]"), std::string::npos) << listener.terminal;
    }
}

TEST(Pty, TheChildStartsWithNoBlockedOrIgnoredSignalAndTheCallerKeepsItsMask)
{
    // A mask and a disposition of the caller: spawn() blocks every signal
    // around fork() and must put this mask back, and the child must not
    // inherit either of them across exec.
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigset_t before;
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, &blocked, &before), 0);
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    struct sigaction previous {};
    ASSERT_EQ(sigaction(SIGUSR2, &ignore, &previous), 0);

    SpawnOptions options;
    options.argv = {"grep", "-E", "^Sig(Blk|Ign):", "/proc/self/status"};
    options.captureOutput = true;
    Listener listener;
    std::string error;
    auto pty = Pty::spawn(std::move(options), listener, &error);

    sigset_t after;
    pthread_sigmask(SIG_SETMASK, &before, &after);
    sigaction(SIGUSR2, &previous, nullptr);
    ASSERT_TRUE(pty) << error;
    EXPECT_EQ(sigismember(&after, SIGUSR1), 1);
    EXPECT_EQ(sigismember(&after, SIGUSR2), 0);
    EXPECT_EQ(sigismember(&after, SIGTERM), 0);
    ASSERT_TRUE(listener.waitClosed());
    std::lock_guard lock(listener.mutex);
    EXPECT_EQ(listener.output, "SigBlk:\t0000000000000000\nSigIgn:\t0000000000000000\n");
}

namespace {

// Whether the effective capabilities of this process (CapEff in
// /proc/self/status) hold CAP_SYS_RESOURCE (24) or CAP_SYS_ADMIN (21).
bool exemptFromProcessLimit()
{
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("CapEff:")) continue;
        const std::uint64_t caps = std::stoull(line.substr(7), nullptr, 16);
        return (caps >> 24 & 1) != 0 || (caps >> 21 & 1) != 0;
    }
    return false;
}

} // namespace

TEST(Pty, AFailedForkReportsWhyAndLeavesNoDescriptor)
{
    // In a process of its own: a soft RLIMIT_NPROC of 0 makes every fork()
    // of this user fail with EAGAIN, and the limit stays with the process.
    // The kernel does not apply the limit to root, nor to a process with
    // CAP_SYS_RESOURCE or CAP_SYS_ADMIN.
    if (geteuid() == 0) GTEST_SKIP() << "RLIMIT_NPROC does not limit root";
    if (exemptFromProcessLimit()) GTEST_SKIP() << "RLIMIT_NPROC does not limit a process with CAP_SYS_RESOURCE or CAP_SYS_ADMIN";
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            rlimit limit{};
            getrlimit(RLIMIT_NPROC, &limit);
            limit.rlim_cur = 0;
            if (setrlimit(RLIMIT_NPROC, &limit) != 0) std::_Exit(2);
            if (const pid_t probe = fork(); probe >= 0) {
                if (probe == 0) std::_Exit(0);
                waitpid(probe, nullptr, 0);
                std::fprintf(stderr, "[a fork works with RLIMIT_NPROC 0: the process has CAP_SYS_RESOURCE or CAP_SYS_ADMIN]\n");
                std::_Exit(3);
            }
            const int before = openDescriptors();
            Listener listener;
            std::string error;
            SpawnOptions options = shell("true");
            options.input = std::string("x");
            options.captureOutput = true;
            const bool spawned = Pty::spawn(std::move(options), listener, &error) != nullptr;
            std::fprintf(stderr, "[%s]\n", error.c_str());
            std::_Exit(!spawned && openDescriptors() == before ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "\\[fork failed: ");
}

TEST(Pty, TheProcessStatusNameCanHoldAnyByte)
{
    // A name with ')', spaces and a new line: the fields follow the last ')'.
    const auto stat = parseProcessStat("4242 (a) b\n(c)) S 1 4242 4200 34817 4300 4194560 0 0");
    ASSERT_TRUE(stat);
    EXPECT_EQ(stat->state, 'S');
    EXPECT_EQ(stat->parent, 1);
    EXPECT_EQ(stat->group, 4242);
    EXPECT_EQ(stat->session, 4200);
    EXPECT_EQ(stat->terminal, 34817);
    EXPECT_EQ(stat->foregroundGroup, 4300);

    EXPECT_FALSE(parseProcessStat(""));
    EXPECT_FALSE(parseProcessStat("4242 (name"));
    EXPECT_FALSE(parseProcessStat("4242 name) S 1 2 3 4 5"));
    EXPECT_FALSE(parseProcessStat("4242 (name) S 1 2"));
}

TEST(Pty, TheProcessStatusOfThisProcessIsReadWithANameThatHoldsANewLine)
{
    // The name of this thread, the main one, is the process's name in
    // /proc/PID/stat: a line read would end inside it.
    char name[16] = {};
    ASSERT_EQ(prctl(PR_GET_NAME, name), 0);
    ASSERT_EQ(prctl(PR_SET_NAME, "t)\n(x) Z 9 9"), 0);
    const auto stat = processStat(::getpid());
    prctl(PR_SET_NAME, name);
    ASSERT_TRUE(stat);
    EXPECT_NE(stat->state, 'Z');
    EXPECT_EQ(stat->parent, ::getppid());
    EXPECT_EQ(stat->group, ::getpgrp());
    EXPECT_EQ(stat->session, ::getsid(0));
}
