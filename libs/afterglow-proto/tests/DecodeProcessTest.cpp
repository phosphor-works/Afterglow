// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/DecodeProcess.h>
#include <AfterglowProto/DecodeRecord.h>
#include <AfterglowProto/Fd.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace AfterglowProto;

namespace {

struct Pipe
{
    UniqueFd reader;
    UniqueFd writer;
    Pipe()
    {
        int fds[2] = {-1, -1};
        if (::pipe2(fds, O_CLOEXEC) == 0) {
            reader.reset(fds[0]);
            writer.reset(fds[1]);
        }
    }
};

bool g_stop = false;
bool stopRequested() { return g_stop; }

} // namespace

TEST(DecodeRecord, FramesAndTextComeThroughInOrder)
{
    Pipe pipe;
    ASSERT_TRUE(pipe.reader.valid());
    const std::string pixels(2 * 3 * 4, '\x7f');
    ASSERT_TRUE(writeDecodeFrame(pipe.writer.get(), 2, 3, 40, pixels.data()));
    ASSERT_TRUE(writeDecodeRecord(pipe.writer.get(), {DecodeRecord::Kind::Warning, 0, 0, 0, 4}, "late"));
    ASSERT_TRUE(writeDecodeRecord(pipe.writer.get(), {DecodeRecord::Kind::End}));
    pipe.writer.reset();

    const DecodeWait wait;
    DecodeRecord record;
    ASSERT_EQ(readDecodeRecord(pipe.reader.get(), record, wait), DecodeRead::Done);
    EXPECT_EQ(record.kind, DecodeRecord::Kind::Frame);
    EXPECT_EQ(record.width, 2u);
    EXPECT_EQ(record.height, 3u);
    EXPECT_EQ(record.delay, 40);
    ASSERT_EQ(record.length, pixels.size());
    std::string read(pixels.size(), '\0');
    ASSERT_EQ(readFromDecoder(pipe.reader.get(), read.data(), read.size(), wait), DecodeRead::Done);
    EXPECT_TRUE(read == pixels);

    ASSERT_EQ(readDecodeRecord(pipe.reader.get(), record, wait), DecodeRead::Done);
    EXPECT_EQ(record.kind, DecodeRecord::Kind::Warning);
    std::string text(std::size_t(record.length), '\0');
    ASSERT_EQ(readFromDecoder(pipe.reader.get(), text.data(), text.size(), wait), DecodeRead::Done);
    EXPECT_EQ(text, "late");

    ASSERT_EQ(readDecodeRecord(pipe.reader.get(), record, wait), DecodeRead::Done);
    EXPECT_EQ(record.kind, DecodeRecord::Kind::End);
    EXPECT_EQ(record.length, 0u);
    // A record cut short is the end of the process.
    EXPECT_EQ(readDecodeRecord(pipe.reader.get(), record, wait), DecodeRead::Ended);
}

TEST(DecodeRecord, AWaitEndsAtItsDeadlineItsCancelAndAStop)
{
    Pipe pipe;
    ASSERT_TRUE(pipe.reader.valid());
    DecodeRecord record;

    DecodeWait late;
    const auto start = std::chrono::steady_clock::now();
    late.deadline = start + std::chrono::milliseconds(50);
    EXPECT_EQ(readDecodeRecord(pipe.reader.get(), record, late), DecodeRead::Late);
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(50));

    // Half a record, then nothing: the deadline still ends the wait.
    ASSERT_TRUE(writeAll(pipe.writer.get(), std::string(sizeof record / 2, '\0')));
    late.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    EXPECT_EQ(readDecodeRecord(pipe.reader.get(), record, late), DecodeRead::Late);

    Pipe cancel;
    ASSERT_TRUE(cancel.reader.valid());
    DecodeWait cancelled;
    cancelled.cancel = cancel.reader.get();
    std::thread closing([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ASSERT_TRUE(writeAll(cancel.writer.get(), "x"));
    });
    EXPECT_EQ(readDecodeRecord(pipe.reader.get(), record, cancelled), DecodeRead::Cancelled);
    closing.join();

    DecodeWait stopped;
    stopped.stopped = stopRequested;
    g_stop = true;
    EXPECT_EQ(readDecodeRecord(pipe.reader.get(), record, stopped), DecodeRead::Interrupted);
    g_stop = false;
}

TEST(DecodeProcess, TheProcessHoldsOnlyItsInputItsOutputAndNullAndIsNotDumpable)
{
    // What the child found, written to its output once it is set up.
    struct Found
    {
        bool input = false; ///< standard input is the given file
        bool nullError = false; ///< standard error is /dev/null
        bool othersClosed = false;
        bool unblocked = false;
        bool notDumpable = false;
        bool defaultInterrupt = false;
        int oomScoreAdj = 0;
    };
    Pipe output;
    ASSERT_TRUE(output.reader.valid());
    UniqueFd input(::open("/proc/self/exe", O_RDONLY | O_CLOEXEC));
    ASSERT_TRUE(input.valid());
    struct stat inputFile{};
    ASSERT_EQ(::fstat(input.get(), &inputFile), 0);
    struct stat null{};
    ASSERT_EQ(::stat("/dev/null", &null), 0);
    // The child must not keep this one: it stands for a terminal or a socket
    // of the parent.
    UniqueFd held(::open("/dev/null", O_RDONLY));
    ASSERT_TRUE(held.valid());
    // A handler of the parent that the child must not run.
    struct sigaction previous{};
    struct sigaction caught{};
    caught.sa_handler = [](int) {};
    ::sigemptyset(&caught.sa_mask);
    ASSERT_EQ(::sigaction(SIGINT, &caught, &previous), 0);
    sigset_t blocked;
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGCHLD);
    sigset_t mask;
    ::sigprocmask(SIG_BLOCK, &blocked, &mask);

    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        if (!startDecodingProcess(parent, input.get(), output.writer.get(), false)) ::_exit(2);
        Found found;
        struct stat file{};
        found.input = ::fstat(STDIN_FILENO, &file) == 0 && file.st_ino == inputFile.st_ino && file.st_dev == inputFile.st_dev;
        found.nullError = ::fstat(STDERR_FILENO, &file) == 0 && S_ISCHR(file.st_mode) && file.st_rdev == null.st_rdev;
        found.othersClosed = ::fcntl(held.get(), F_GETFD) < 0 && ::fcntl(STDERR_FILENO + 1, F_GETFD) < 0;
        sigset_t now;
        found.unblocked = ::sigprocmask(SIG_SETMASK, nullptr, &now) == 0 && !::sigismember(&now, SIGCHLD);
        found.notDumpable = ::prctl(PR_GET_DUMPABLE) == 0;
        struct sigaction interrupt{};
        found.defaultInterrupt = ::sigaction(SIGINT, nullptr, &interrupt) == 0 && interrupt.sa_handler == SIG_DFL;
        // /proc/self belongs to root now: the score is not readable to all.
        found.oomScoreAdj = -1;
        if (const int fd = ::open("/proc/self/oom_score_adj", O_RDONLY); fd >= 0) {
            char text[16] = {};
            if (::read(fd, text, sizeof text - 1) > 0) found.oomScoreAdj = std::atoi(text);
            ::close(fd);
        }
        (void)!::write(STDOUT_FILENO, &found, sizeof found);
        ::_exit(0);
    }
    ::sigprocmask(SIG_SETMASK, &mask, nullptr);
    ::sigaction(SIGINT, &previous, nullptr);
    output.writer.reset();
    Found found;
    const DecodeWait wait;
    EXPECT_EQ(readFromDecoder(output.reader.get(), &found, sizeof found, wait), DecodeRead::Done);
    int status = -1;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << status;
    EXPECT_TRUE(found.input);
    EXPECT_TRUE(found.nullError);
    EXPECT_TRUE(found.othersClosed);
    EXPECT_TRUE(found.unblocked);
    EXPECT_TRUE(found.notDumpable);
    EXPECT_TRUE(found.defaultInterrupt);
    // Unreadable (-1) when the kernel gave /proc/self to root at once.
    if (found.oomScoreAdj != -1) {
        EXPECT_EQ(found.oomScoreAdj, 1000);
    }
}

// A parent that started without standard error can hand its output over in
// descriptor 2: the sanitizer build's standard error must not stay there.
TEST(DecodeProcess, AnOutputInStandardErrorLeavesNoStandardErrorThere)
{
    Pipe output;
    ASSERT_TRUE(output.reader.valid());
    struct stat null{};
    ASSERT_EQ(::stat("/dev/null", &null), 0);
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        if (::dup2(output.writer.get(), STDERR_FILENO) < 0) ::_exit(3);
        if (!startDecodingProcess(parent, -1, STDERR_FILENO, true)) ::_exit(2);
        struct stat file{};
        const bool nullError = ::fstat(STDERR_FILENO, &file) == 0 && S_ISCHR(file.st_mode) && file.st_rdev == null.st_rdev;
        (void)!::write(STDOUT_FILENO, &nullError, sizeof nullError);
        ::_exit(0);
    }
    output.writer.reset();
    bool nullError = false;
    const DecodeWait wait;
    EXPECT_EQ(readFromDecoder(output.reader.get(), &nullError, sizeof nullError, wait), DecodeRead::Done);
    int status = -1;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << status;
    EXPECT_TRUE(nullError);
}
