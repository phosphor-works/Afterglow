// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowPty/Pty.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
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
    bool waitClosed()
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 10s, [this] { return closed; });
    }
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
        // No callback before start(): a child that ran would have echoed
        // its input and exited by the end of this wait.
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
