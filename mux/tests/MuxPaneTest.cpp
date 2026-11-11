// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// A pane of the daemon while its program's image decodes: the daemon's one
// thread does not wait for the decode.

#include "ImageDecoder.h"
#include "ImageFiles.h"
#include "MuxPane.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowPty/Pty.h>
#include <AfterglowVt/StateSync.h>
#include <AfterglowVt/Terminal.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

using AfterglowMux::ImageDecoder;
using AfterglowMux::MuxPane;
using AfterglowMux::PaneSize;
using Clock = std::chrono::steady_clock;

namespace {

struct NullHost : AfterglowVt::ITerminalHost
{
    void writeToPty(std::string_view) override {}
};

/// A directory of its own, removed however the test ends.
struct TempDir
{
    std::filesystem::path path;
    TempDir()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "afterglow-mux-pane-XXXXXX").string();
        if (::mkdtemp(pattern.data())) path = pattern;
    }
    ~TempDir()
    {
        std::error_code ignored;
        if (!path.empty()) std::filesystem::remove_all(path, ignored);
    }
};

/// A pane whose program prints a PNG with kitty graphics and then runs
/// @p then (by default, it waits), with a decoder whose server takes the request and never answers: the
/// decode lasts until the decoder's wait ends.
struct StuckDecode
{
    TempDir dir;
    std::unique_ptr<ImageDecoder> decoder;
    std::unique_ptr<MuxPane> pane;
    AfterglowVt::StateWriter writer;

    explicit StuckDecode(int waitMs, const std::string& then = "exec /bin/sleep 60")
    {
        if (dir.path.empty()) return;
        const std::string server = (dir.path / "server").string();
        std::ofstream(server) << "#!/bin/sh\nexec /bin/sleep 60\n";
        if (::chmod(server.c_str(), 0700) != 0) return;
        const std::string image = (dir.path / "image").string();
        std::ofstream(image) << "\x1b_Ga=T,f=100,i=1,q=2;" << AfterglowProto::base64Encode(MuxTest::Images::Png) << "\x1b\\";
        decoder = std::make_unique<ImageDecoder>(server, waitMs);
        pane = std::make_unique<MuxPane>(1, AfterglowVt::TerminalOptions{}, PaneSize{80, 24, 10, 20}, std::string(), *decoder,
                                         [] {});
        AfterglowPty::SpawnOptions spawn;
        spawn.argv = {"/bin/sh", "-c", "cat '" + image + "'; " + then};
        std::string error;
        if (!pane->start(std::move(spawn), std::string(), &error)) pane.reset();
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
};

} // namespace

TEST(MuxPane, TheDaemonDoesNotWaitForADecodeAndItsChangesComeAfter)
{
    StuckDecode stuck(1500);
    ASSERT_TRUE(stuck.pane);
    ASSERT_TRUE(stuck.decoding());
    // The calls of the daemon's thread return at once, and the frame keeps
    // waiting for the decode.
    const auto start = Clock::now();
    stuck.pane->resize(PaneSize{100, 30, 10, 20});
    stuck.pane->setVisible(true, 0);
    stuck.pane->clear();
    EXPECT_LT(Clock::now() - start, std::chrono::milliseconds(500));
    EXPECT_FALSE(stuck.pane->frame(stuck.writer, std::size_t(1) << 20));

    // The decode ends with the decoder's wait: the feed makes the changes,
    // and the next frame carries them.
    AfterglowVt::StateWriter fresh;
    std::optional<std::string> frame;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline && !(frame = stuck.pane->frame(fresh, std::size_t(1) << 20))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(frame);
    NullHost host;
    AfterglowVt::Terminal replica(80, 24, host);
    AfterglowVt::StateReader reader;
    std::string error;
    ASSERT_TRUE(reader.apply(replica, *frame, &error)) << error;
    EXPECT_EQ(replica.cols(), 100);
    EXPECT_EQ(replica.rows(), 30);
}

TEST(MuxPane, InputWaitsInThePaneBehindAChangeThatWaitsForADecode)
{
    // The program reads a line, then writes the size of its terminal.
    TempDir out;
    ASSERT_FALSE(out.path.empty());
    const std::string size = (out.path / "size").string();
    StuckDecode stuck(1500, "IFS= read -r line; stty size > '" + size + "'; exec /bin/sleep 60");
    ASSERT_TRUE(stuck.pane);
    ASSERT_TRUE(stuck.decoding());
    // The pane takes the input behind the resize, and the window's other
    // panes go on. Past half of the PTY's queue, the window holds it.
    stuck.pane->resize(PaneSize{100, 30, 10, 20});
    EXPECT_EQ(stuck.pane->input("x\n"), MuxPane::Input::Written);
    EXPECT_EQ(stuck.pane->input(std::string(AfterglowPty::Pty::MaxPendingInput / 2, 'a')), MuxPane::Input::Full);
    // The decode ends: the resize goes first, then the line.
    std::string got;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline && got.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::ifstream file(size);
        std::getline(file, got);
    }
    EXPECT_EQ(got, "30 100");
}

TEST(MuxPane, APaneThatClosesDoesNotWaitForItsDecode)
{
    StuckDecode stuck(30000);
    ASSERT_TRUE(stuck.pane);
    ASSERT_TRUE(stuck.decoding());
    const auto start = Clock::now();
    stuck.pane.reset();
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
}
