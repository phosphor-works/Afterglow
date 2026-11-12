// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// A pane of the daemon while its program's image decodes: the daemon's one
// thread does not wait for the decode.

#include "ImageDecoder.h"
#include "ImageFiles.h"
#include "MuxPane.h"
#include "TestDaemon.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowPty/Pty.h>
#include <AfterglowVt/StateSync.h>
#include <AfterglowVt/Terminal.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <unistd.h>

using AfterglowMux::ImageDecoder;
using AfterglowMux::MuxPane;
using AfterglowMux::PaneSize;
using Clock = std::chrono::steady_clock;
using MuxTest::readFile;
using MuxTest::TempDir;

namespace {

struct NullHost : AfterglowVt::ITerminalHost
{
    void writeToPty(std::string_view) override {}
};

/// The servers of decoders: the children of this process that name
/// themselves afterglow-image once they run.
std::set<pid_t> imageServers()
{
    std::set<pid_t> found;
    std::error_code ignored;
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task", ignored)) {
        std::ifstream list(task.path() / "children");
        for (pid_t pid = 0; list >> pid;) {
            if (readFile("/proc/" + std::to_string(pid) + "/comm") == "afterglow-image\n") found.insert(pid);
        }
    }
    return found;
}

/// The server that started after @p before: 0 when none does in a few
/// seconds, or the kernel lists no children.
pid_t newImageServer(const std::set<pid_t>& before)
{
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    do {
        for (const pid_t pid : imageServers()) {
            if (!before.contains(pid)) return pid;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (Clock::now() < deadline && MuxTest::childrenListed());
    return 0;
}

/// A pane whose program prints a PNG with kitty graphics and then runs
/// @p then (by default, it waits). The decoder's server is stopped
/// (SIGSTOP): it takes the request and answers when the test lets it go on
/// (release()), or never, and then the decode lasts until the decoder's
/// wait ends. @p before runs before the PNG. The program runs in an
/// environment of its own, with nothing of the user's session.
struct StuckDecode
{
    TempDir dir;
    std::unique_ptr<ImageDecoder> decoder;
    std::unique_ptr<MuxPane> pane;
    AfterglowVt::StateWriter writer;
    std::atomic<int> wakes = 0;
    pid_t server = 0;

    /// @p after follows the image in the same write, which the pane reads at once.
    explicit StuckDecode(int waitMs, const std::string& then = "exec /bin/sleep 60", const std::string& before = {},
                         const std::string& after = {})
    {
        if (dir.path.empty()) return;
        const std::string image = (dir.path / "image").string();
        std::ofstream(image) << "\x1b_Ga=T,f=100,i=1,q=2;" << AfterglowProto::base64Encode(AfterglowImage::TestFiles::Png) << "\x1b\\"
                             << after;
        // Other tests of this program may have a server running.
        const std::set<pid_t> running = imageServers();
        decoder = std::make_unique<ImageDecoder>(AFTERGLOW_MUX, waitMs);
        server = newImageServer(running);
        if (server <= 0 || ::kill(server, SIGSTOP) != 0) return;
        pane = std::make_unique<MuxPane>(1, AfterglowVt::TerminalOptions{}, PaneSize{80, 24, 10, 20}, std::string(), *decoder,
                                         [this] { ++wakes; });
        AfterglowPty::SpawnOptions spawn;
        spawn.argv = {"/bin/sh", "-c", before + "cat '" + image + "'; " + then};
        spawn.inheritEnvironment = false;
        spawn.environment = {{"PATH", "/usr/bin:/bin"}, {"HOME", dir.path.string()}, {"TERM", "xterm-256color"}};
        std::string error;
        if (!pane->start(std::move(spawn), std::string(), &error)) pane.reset();
    }
    ~StuckDecode()
    {
        // The pane ends before its decoder, and a stopped server ends too.
        pane.reset();
        release();
    }

    /// The server goes on: the decode that waits for it ends.
    void release() const
    {
        if (server > 0) ::kill(server, SIGCONT);
    }
    /// The server stops again: the next decode waits.
    void hold() const
    {
        if (server > 0) ::kill(server, SIGSTOP);
    }

    /// Waits until a frame waits for the decode: the program's image is in
    /// the decoder. False when it never is.
    bool decoding()
    {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < deadline) {
            if (!pane->frame(writer, std::size_t(1) << 20)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    /// Waits until the pane woke the daemon more than @p before times.
    bool wokeAfter(int before) const
    {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (wakes <= before && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return wakes > before;
    }
};

// Only a kernel that lists no children skips: a server that does not start
// or does not name itself is a failure.
#define SKIP_WITHOUT_SERVER(stuck)                                                                                  \
    if (!MuxTest::childrenListed()) GTEST_SKIP() << "the kernel lists no children of a process (CONFIG_PROC_CHILDREN)"; \
    ASSERT_FALSE((stuck).dir.path.empty());                                                                         \
    ASSERT_GT((stuck).server, 0) << "the decoder's server did not start, or did not name itself afterglow-image"

} // namespace

TEST(MuxPane, TheDaemonDoesNotWaitForADecodeAndItsChangesComeAfter)
{
    StuckDecode stuck(30000);
    SKIP_WITHOUT_SERVER(stuck);
    ASSERT_TRUE(stuck.pane);
    ASSERT_TRUE(stuck.decoding());
    // The calls of the daemon's thread return at once, and the frame keeps
    // waiting for the decode, also after a change.
    const auto start = Clock::now();
    stuck.pane->resize(PaneSize{100, 30, 10, 20});
    stuck.pane->clear();
    EXPECT_LT(Clock::now() - start, std::chrono::milliseconds(500));
    EXPECT_FALSE(stuck.pane->frame(stuck.writer, std::size_t(1) << 20));

    // The decode ends: the feed makes the changes, and wakes the daemon,
    // whose next frame carries them. Without the wake the daemon would not
    // look again: the flag of a change is set already.
    const int before = stuck.wakes;
    stuck.release();
    ASSERT_TRUE(stuck.wokeAfter(before));
    AfterglowVt::StateWriter fresh;
    const std::optional<std::string> frame = stuck.pane->frame(fresh, std::size_t(1) << 20);
    ASSERT_TRUE(frame);
    NullHost host;
    AfterglowVt::Terminal replica(80, 24, host);
    AfterglowVt::StateReader reader;
    std::string error;
    ASSERT_TRUE(reader.apply(replica, *frame, &error)) << error;
    EXPECT_EQ(replica.cols(), 100);
    EXPECT_EQ(replica.rows(), 30);
}

TEST(MuxPane, VisibilityWaitsForADecode)
{
    // The program asks for reports of its visibility (mode 2033) before
    // its image, and rings the bell after it, in the same read. The pane's
    // window shows it while the image decodes: the report comes after the
    // decode, in its order with the program's output, so after the bell.
    StuckDecode stuck(30000, "exec /bin/sleep 60", "printf '\\033[?2033h'; ", "\a");
    SKIP_WITHOUT_SERVER(stuck);
    ASSERT_TRUE(stuck.pane);
    stuck.pane->setWindow(1, std::size_t(1) << 20);
    ASSERT_TRUE(stuck.decoding());
    stuck.pane->setVisible(true, 1);
    // As the daemon does, a frame before the calls.
    std::vector<std::string> calls;
    const auto take = [&stuck, &calls] {
        (void)stuck.pane->frame(stuck.writer, std::size_t(1) << 20);
        for (std::string& call : stuck.pane->takeHostCalls(1, std::size_t(1) << 20)) calls.push_back(std::move(call));
    };
    // The bell by its whole call, and the report in the bytes of its call.
    const auto at = [&calls](const std::string& wanted, bool whole) {
        for (std::size_t i = 0; i < calls.size(); ++i) {
            if (whole ? calls[i] == wanted : calls[i].find(wanted) != std::string::npos) return std::ptrdiff_t(i);
        }
        return std::ptrdiff_t(-1);
    };
    const std::string report = "\x1b[?999;1n";
    const std::string bell = AfterglowMux::Host::encode(AfterglowMux::Host::make(AfterglowMux::Host::Kind::Bell));
    take();
    EXPECT_EQ(at(report, false), -1);
    stuck.release();
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while ((at(report, false) < 0 || at(bell, true) < 0) && Clock::now() < deadline) {
        take();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GE(at(report, false), 0);
    ASSERT_GE(at(bell, true), 0);
    EXPECT_LT(at(bell, true), at(report, false));
}

TEST(MuxPane, InputWaitsInThePaneBehindAChangeThatWaitsForADecode)
{
    // The program marks a prompt, prints its image, reads a line, then
    // writes the size of its terminal and the line.
    TempDir out;
    ASSERT_FALSE(out.path.empty());
    const std::string size = (out.path / "size").string();
    const std::string line = (out.path / "line").string();
    StuckDecode stuck(30000, "IFS= read -r line; stty size > '" + size + "'; printf '%s' \"$line\" > '" + line + "'; exec /bin/sleep 60",
                      "printf '\\033]133;A\\007'; ");
    SKIP_WITHOUT_SERVER(stuck);
    ASSERT_TRUE(stuck.pane);
    ASSERT_TRUE(stuck.decoding());
    // The pane takes the input behind the resize and the clear, and the
    // window's other panes go on. Past half of the PTY's queue, the window
    // holds it: here by the input alone, with nothing waiting before it.
    stuck.pane->resize(PaneSize{100, 30, 10, 20});
    stuck.pane->clear();
    EXPECT_EQ(stuck.pane->input(std::string(AfterglowPty::Pty::MaxPendingInput / 2 + 1, 'a')), MuxPane::Input::Full);
    EXPECT_EQ(stuck.pane->input("x\n"), MuxPane::Input::Written);
    // The decode ends: the changes go first, then the line, and the daemon
    // is woken for the input that it holds. The clear at a prompt writes a
    // form feed for the shell, which the line follows: the PTY's queue has
    // it first (written at once, the line would be first).
    const int before = stuck.wakes;
    stuck.release();
    EXPECT_TRUE(stuck.wokeAfter(before));
    std::string got;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline && (got = readFile(line)).empty()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(got, "\x0cx");
    EXPECT_EQ(readFile(size), "30 100\n");
}

TEST(MuxPane, TheBoundOfWaitingInputCountsThePtyQueueAndWhatWent)
{
    // The program reads nothing: what the pane writes stays in the PTY's
    // queue. It prints its image again once the test says so.
    TempDir out;
    ASSERT_FALSE(out.path.empty());
    const std::string next = (out.path / "next").string();
    const std::string image = (out.path / "image").string();
    std::ofstream(image) << "\x1b_Ga=T,f=100,i=2,q=2;" << AfterglowProto::base64Encode(AfterglowImage::TestFiles::Png) << "\x1b\\";
    StuckDecode stuck(30000, "while [ ! -e '" + next + "' ]; do sleep 0.02; done; cat '" + image + "'; exec /bin/sleep 60");
    SKIP_WITHOUT_SERVER(stuck);
    ASSERT_TRUE(stuck.pane);
    ASSERT_TRUE(stuck.decoding());
    const std::size_t half = AfterglowPty::Pty::MaxPendingInput / 2;
    // Input that waits behind a change, a little less than half of the
    // queue: it goes to the PTY when the decode ends.
    stuck.pane->resize(PaneSize{100, 30, 10, 20});
    EXPECT_EQ(stuck.pane->input(std::string(half - 4096, 'a')), MuxPane::Input::Written);
    const int before = stuck.wakes;
    stuck.release();
    ASSERT_TRUE(stuck.wokeAfter(before));
    // The second image waits too. What went counts no more as waiting, so
    // a small input waits behind a change again. Beside the PTY's queue,
    // which still holds what went less what the kernel took (some 64 KiB),
    // a mebibyte more does not fit.
    stuck.hold();
    std::ofstream(next) << "";
    ASSERT_TRUE(stuck.decoding());
    stuck.pane->resize(PaneSize{80, 24, 10, 20});
    EXPECT_EQ(stuck.pane->input("x"), MuxPane::Input::Written);
    EXPECT_EQ(stuck.pane->input(std::string(std::size_t(1) << 20, 'b')), MuxPane::Input::Full);
}

TEST(MuxPane, APaneThatClosesDoesNotWaitForItsDecode)
{
    StuckDecode stuck(30000);
    SKIP_WITHOUT_SERVER(stuck);
    ASSERT_TRUE(stuck.pane);
    ASSERT_TRUE(stuck.decoding());
    const auto start = Clock::now();
    stuck.pane.reset();
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
}
