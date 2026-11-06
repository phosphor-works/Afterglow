// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowPty/Pty.h>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#include <sys/wait.h>

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

    void ptyData(std::string_view bytes) override
    {
        std::lock_guard lock(mutex);
        terminal.append(bytes);
        changed.notify_all();
    }
    void ptyOutput(std::string_view bytes) override
    {
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
