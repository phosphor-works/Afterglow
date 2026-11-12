// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The image decoder of the daemon: the server aglow-mux --decode-images,
// and a process that it forks for each PNG and iTerm2 image.

#include "ImageDecoder.h"
#include "ImageFiles.h"
#include "TestDaemon.h"

#include <AfterglowProto/Zlib.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using AfterglowMux::ImageDecoder;
using AfterglowMux::ImageFormats;
using namespace AfterglowImage::TestFiles;
using MuxTest::childrenListed;
using MuxTest::TempDir;
using MuxTest::greyPng;
using Clock = std::chrono::steady_clock;

namespace {

struct Decoded
{
    bool ok = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

Decoded decode(ImageDecoder& decoder, std::string_view data, ImageFormats formats, int cancel = -1)
{
    Decoded d;
    d.ok = decoder.decode(data, formats, d.width, d.height, d.rgba, cancel);
    return d;
}

/// The daemon's decoder: one server for the tests, as for a daemon.
Decoded decode(std::string_view data, ImageFormats formats)
{
    static ImageDecoder decoder(AFTERGLOW_MUX);
    return decode(decoder, data, formats);
}

/// The processes that @p parent started: for this process, the servers of
/// the decoders, and for a server, its decoding processes.
std::set<pid_t> children(pid_t parent = ::getpid())
{
    std::set<pid_t> found;
    std::error_code ignored;
    for (const auto& task : std::filesystem::directory_iterator("/proc/" + std::to_string(parent) + "/task", ignored)) {
        std::ifstream list(task.path() / "children");
        for (pid_t pid = 0; list >> pid;) found.insert(pid);
    }
    return found;
}

/// The one process that is in @p after and not in @p before, or 0.
pid_t added(const std::set<pid_t>& before, const std::set<pid_t>& after)
{
    std::vector<pid_t> found;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(found));
    return found.size() == 1 ? found[0] : 0;
}

/// The lines of /proc/@p pid/@p file that start with @p prefix, joined.
std::string procLines(pid_t pid, const char* file, const char* prefix)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/" + file);
    std::string out;
    for (std::string line; std::getline(in, line);) {
        if (line.starts_with(prefix)) out += line + '\n';
    }
    return out;
}

#define SKIP_WITHOUT(script)                                                                                        \
    if ((script).empty()) GTEST_SKIP() << "the temporary directory runs no program (noexec)"

} // namespace

TEST(MuxImageDecoder, APngDecodesToItsPixelsWithStraightAlpha)
{
    for (const ImageFormats formats : {ImageFormats::Png, ImageFormats::Common}) {
        const Decoded d = decode(Png, formats);
        EXPECT_TRUE(d.ok) << int(formats);
        if (!d.ok) continue;
        EXPECT_EQ(d.width, 3u);
        EXPECT_EQ(d.height, 2u);
        const std::vector<std::uint8_t> expected{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0,
                                                 10, 20, 30, 255, 200, 100, 50, 255, 1, 2, 3, 4};
        EXPECT_EQ(d.rgba, expected);
    }
}

TEST(MuxImageDecoder, AnItermImageCanBeJpegGifWebPOrBmp)
{
    Decoded d = decode(Bmp, ImageFormats::Common);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.width, 2u);
    EXPECT_EQ(d.height, 2u);
    EXPECT_EQ(d.rgba, (std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}));
    // The first frame of an animation, as the window shows it.
    d = decode(Gif, ImageFormats::Common);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.width, 4u);
    EXPECT_EQ(d.height, 4u);
    EXPECT_EQ(d.rgba, repeat({255, 0, 0, 255}, 16));
    d = decode(WebpAnimated, ImageFormats::Common);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.width, 4u);
    EXPECT_EQ(d.height, 4u);
    EXPECT_EQ(d.rgba, repeat({255, 0, 0, 255}, 16));
    d = decode(Webp, ImageFormats::Common);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.width, 2u);
    EXPECT_EQ(d.height, 2u);
    EXPECT_EQ(d.rgba, repeat({255, 0, 0, 255}, 4));
    d = decode(Jpeg, ImageFormats::Common);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.width, 8u);
    EXPECT_EQ(d.height, 8u);
    ASSERT_EQ(d.rgba.size(), 8u * 8 * 4);
    int notRed = 0;
    for (std::size_t i = 0; i < d.rgba.size(); i += 4) {
        if (d.rgba[i] <= 240 || d.rgba[i + 1] >= 16 || d.rgba[i + 2] >= 16 || d.rgba[i + 3] != 255) ++notRed;
    }
    EXPECT_EQ(notRed, 0);
}

TEST(MuxImageDecoder, RefusesOtherFormatsAndBadFiles)
{
    // kitty's f=100 is PNG only.
    for (const std::string* file : {&Jpeg, &Gif, &Bmp, &Webp}) EXPECT_FALSE(decode(*file, ImageFormats::Png).ok);
    // Formats that no decoder here reads (PPM, TGA): their signature is unknown.
    // Corrupt files of the formats that it reads are the library's tests
    // (Image.ImageDecode.*): there a sanitizer's report fails the test,
    // where here it ends a decoding process as a refusal does.
    EXPECT_FALSE(decode(std::string("P6\n1 1\n255\n\xff\x00\x00", 14), ImageFormats::Common).ok);
    const std::string tga("\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x01\x00\x18\x00\x00\x00\xff", 21);
    EXPECT_FALSE(decode(tga, ImageFormats::Common).ok);
    EXPECT_FALSE(decode("", ImageFormats::Common).ok);
    EXPECT_FALSE(decode("not an image at all", ImageFormats::Common).ok);
    EXPECT_FALSE(decode(Png.substr(0, 40), ImageFormats::Png).ok);
    // A side past what a terminal takes: refused from the header, though
    // the file is whole.
    EXPECT_FALSE(decode(greyPng(10001, 1), ImageFormats::Png).ok);
    // Pixels past MaxBytes, with each side within it: 10,000 by 8,389 is
    // 335,560,000 bytes, 15,680 more than the 320 MiB.
    EXPECT_FALSE(decode(greyPng(10000, 8389), ImageFormats::Png).ok);
    // Apple's CgBI: a chunk before IHDR.
    EXPECT_FALSE(decode(cgbi(), ImageFormats::Common).ok);
    // The next decode works: nothing of the failures stays.
    EXPECT_TRUE(decode(Png, ImageFormats::Png).ok);
}

TEST(MuxImageDecoder, ASideOfTenThousandDecodes)
{
    // The bound itself, which the refusal above passes by one.
    const Decoded d = decode(greyPng(10000, 1), ImageFormats::Png);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.width, 10000u);
    EXPECT_EQ(d.height, 1u);
}

TEST(MuxImageDecoder, AServerThatIsMissingOrEndsDecodesNothing)
{
    {
        // A server that is missing does not start again for a minute: also
        // not when it is there by the next decode.
        TempDir dir;
        ASSERT_FALSE(dir.path.empty());
        const std::string path = (dir.path / "later").string();
        ImageDecoder missing(path);
        EXPECT_FALSE(decode(missing, Png, ImageFormats::Png).ok);
        const std::string starts = (dir.path / "later-starts").string();
        const std::string later = dir.script("later", "echo started >> '" + starts + "'\nexit 1");
        SKIP_WITHOUT(later);
        EXPECT_FALSE(decode(missing, Png, ImageFormats::Png).ok);
        EXPECT_FALSE(std::filesystem::exists(starts));
    }
    // A server that ends before it answers once: the decode starts one more.
    // When that one ends before it answers too, the server cannot start:
    // the decoder does not start it again for a minute, also not for the
    // next images.
    TempDir dir;
    ASSERT_FALSE(dir.path.empty());
    const std::string starts = (dir.path / "starts").string();
    const std::string ends = dir.script("ends", "echo started >> '" + starts + "'\nexit 1");
    SKIP_WITHOUT(ends);
    ImageDecoder decoder(ends);
    for (int i = 0; i < 3; ++i) EXPECT_FALSE(decode(decoder, Png, ImageFormats::Png).ok);
    std::ifstream log(starts);
    int count = 0;
    for (std::string line; std::getline(log, line);) ++count;
    EXPECT_EQ(count, 2);
}

TEST(MuxImageDecoder, AServerKilledBeforeItsFirstImageStartsAgain)
{
    // The server ran, and the OOM killer or a user ended it before any
    // image came: it did start, so the next decode starts another.
    if (!childrenListed()) GTEST_SKIP() << "the kernel lists no children of a process (CONFIG_PROC_CHILDREN)";
    const std::set<pid_t> before = children();
    ImageDecoder decoder(AFTERGLOW_MUX);
    const pid_t first = added(before, children());
    ASSERT_GT(first, 0);
    ASSERT_EQ(::kill(first, SIGKILL), 0);
    for (int i = 0; i < 200 && procLines(first, "status", "State:").find('Z') == std::string::npos; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(decode(decoder, Png, ImageFormats::Png).ok);
    const pid_t second = added(before, children());
    EXPECT_GT(second, 0);
    EXPECT_NE(second, first);
}

TEST(MuxImageDecoder, AServerThatEndsLaterStartsAgain)
{
    // The server ended after it answered: a crash, the OOM killer. The next
    // decode starts another, which decodes.
    if (!childrenListed()) GTEST_SKIP() << "the kernel lists no children of a process (CONFIG_PROC_CHILDREN)";
    const std::set<pid_t> before = children();
    ImageDecoder decoder(AFTERGLOW_MUX);
    ASSERT_TRUE(decode(decoder, Png, ImageFormats::Png).ok);
    const pid_t first = added(before, children());
    ASSERT_GT(first, 0);
    ASSERT_EQ(::kill(first, SIGKILL), 0);
    // It is the decoder's child: a zombie until the decoder reaps it.
    for (int i = 0; i < 200 && procLines(first, "status", "State:").find('Z') == std::string::npos; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(decode(decoder, Png, ImageFormats::Png).ok);
    const pid_t second = added(before, children());
    EXPECT_GT(second, 0);
    EXPECT_NE(second, first);
}

TEST(MuxImageDecoder, AServerThatDoesNotAnswerEndsAtTheDeadline)
{
    // A server that sleeps the first time it runs, and is the real one
    // after: the decoder stops the stuck one at the deadline, and the next
    // decode starts one that decodes.
    TempDir dir;
    ASSERT_FALSE(dir.path.empty());
    const std::string once = (dir.path / "once").string();
    const std::string stuck = dir.script("stuck", "if [ ! -e '" + once + "' ]; then : > '" + once + "'; exec /bin/sleep 60; fi\nexec '"
                                                      + AFTERGLOW_MUX + "' \"$@\"");
    SKIP_WITHOUT(stuck);
    ImageDecoder decoder(stuck, 300);
    const auto start = Clock::now();
    EXPECT_FALSE(decode(decoder, Png, ImageFormats::Png).ok);
    const auto took = Clock::now() - start;
    // It waited for the server, which ran: a script that could not run
    // would fail at once.
    EXPECT_GE(took, std::chrono::milliseconds(300));
    EXPECT_LT(took, std::chrono::seconds(10));
    // A server that was stuck is no failed start: the next one runs. Its
    // start counts in the deadline, which a loaded machine can pass.
    bool decoded = false;
    for (int attempt = 0; attempt < 5 && !decoded; ++attempt) decoded = decode(decoder, Png, ImageFormats::Png).ok;
    EXPECT_TRUE(decoded);
}

TEST(MuxImageDecoder, ADecodeThatWaitsForAnotherEndsWhenItsPaneCloses)
{
    // One pane's request waits 3 s for a server that does not answer. A
    // second pane waits behind it, and closes: its decode ends at once, not
    // when the first one's wait ends.
    TempDir dir;
    ASSERT_FALSE(dir.path.empty());
    const std::string stuck = dir.script("stuck", "exec /bin/sleep 60");
    SKIP_WITHOUT(stuck);
    ImageDecoder decoder(stuck, 3000);
    std::thread first([&decoder] { EXPECT_FALSE(decode(decoder, Png, ImageFormats::Png).ok); });
    // The first request holds the server by now.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int cancel = ::eventfd(0, EFD_CLOEXEC);
    ASSERT_GE(cancel, 0);
    std::thread closes([cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::uint64_t one = 1;
        (void)!::write(cancel, &one, sizeof one);
    });
    const auto start = Clock::now();
    EXPECT_FALSE(decode(decoder, Png, ImageFormats::Png, cancel).ok);
    const auto took = Clock::now() - start;
    closes.join();
    first.join();
    ::close(cancel);
    EXPECT_LT(took, std::chrono::milliseconds(1500));
}

TEST(MuxImageDecoder, TheServerTakesOnlyItsDaemon)
{
    // aglow-mux --decode-images runs only as the child of the daemon
    // whose process id it gets: pid 1 is not its parent.
    pid_t pid = 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    const std::string argument(AfterglowMux::DecodeServerArgument);
    char* const argv[] = {const_cast<char*>(AFTERGLOW_MUX), const_cast<char*>(argument.c_str()), const_cast<char*>("1"), nullptr};
    char* const env[] = {nullptr};
    ASSERT_EQ(::posix_spawn(&pid, AFTERGLOW_MUX, &actions, nullptr, argv, env), 0);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 1) << status;
}

TEST(MuxImageDecoder, ADecodingProcessHasItsLimitsAndItsFilter)
{
    // While a large image decodes, its process: no new privileges, the
    // filter of its system calls, and limits on its memory, its CPU time
    // and its core file.
    if (!childrenListed()) GTEST_SKIP() << "the kernel lists no children of a process (CONFIG_PROC_CHILDREN)";
    const std::string large = greyPng(8000, 8000);
    const std::set<pid_t> before = children();
    ImageDecoder decoder(AFTERGLOW_MUX);
    ASSERT_TRUE(decode(decoder, Png, ImageFormats::Png).ok);
    const pid_t server = added(before, children());
    ASSERT_GT(server, 0);
    std::string status;
    std::string limits;
    std::thread watch([&] {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < deadline) {
            for (const pid_t process : children(server)) {
                const std::string seen = procLines(process, "status", "Seccomp:") + procLines(process, "status", "NoNewPrivs:");
                // Read once its filter is on: the limits are set before it.
                if (seen.find("Seccomp:\t2") != std::string::npos) {
                    status = seen;
                    limits = procLines(process, "limits", "Max");
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const Decoded d = decode(decoder, large, ImageFormats::Png);
    watch.join();
    EXPECT_TRUE(d.ok);
    ASSERT_FALSE(status.empty()) << "no decoding process was seen with its filter";
    EXPECT_NE(status.find("NoNewPrivs:\t1"), std::string::npos) << status;
    const auto line = [&limits](const char* name) {
        const std::size_t at = limits.find(name);
        return at == std::string::npos ? std::string() : limits.substr(at, limits.find('\n', at) - at);
    };
    EXPECT_EQ(line("Max address space").find("unlimited"), std::string::npos) << limits;
    EXPECT_EQ(line("Max cpu time").find("unlimited"), std::string::npos) << limits;
    EXPECT_NE(line("Max core file size").find(" 0 "), std::string::npos) << limits;
}

TEST(MuxImageDecoder, PanesDecodeAtOnce)
{
    // The PTY reader threads of the panes call decode() at once: each gets
    // the pixels of its own image.
    ImageDecoder decoder(AFTERGLOW_MUX);
    std::vector<std::thread> panes;
    std::atomic<int> wrong = 0;
    for (int pane = 0; pane < 4; ++pane) {
        panes.emplace_back([&decoder, &wrong, pane] {
            for (int i = 0; i < 20; ++i) {
                const std::string& file = (pane + i) % 3 == 0 ? Png : (pane + i) % 3 == 1 ? Gif : Bmp;
                const Decoded d = decode(decoder, file, ImageFormats::Common);
                const std::uint32_t side = &file == &Png ? 3 : &file == &Gif ? 4 : 2;
                if (!d.ok || d.width != side) ++wrong;
            }
        });
    }
    for (auto& pane : panes) pane.join();
    EXPECT_EQ(wrong, 0);
}

TEST(MuxImageDecoder, ADecodeEndsAtItsDeadlineOrWhenItIsCancelled)
{
    // 8,000 by 8,000, which it does decode. The waits below are a third of
    // the time that the whole decode takes here.
    const std::string large = greyPng(8000, 8000);
    ImageDecoder decoder(AFTERGLOW_MUX);
    // The server runs before the measure: the time is the decode's alone.
    // The shorter of two decodes: other tests that run at once can make one
    // slow, and the waits below must be shorter than a decode.
    ASSERT_TRUE(decode(decoder, Png, ImageFormats::Png).ok);
    auto full = Clock::duration::max();
    auto start = Clock::now();
    for (int i = 0; i < 2; ++i) {
        start = Clock::now();
        const Decoded whole = decode(decoder, large, ImageFormats::Png);
        ASSERT_TRUE(whole.ok);
        EXPECT_EQ(whole.width, 8000u);
        full = std::min(full, Clock::now() - start);
    }

    // The server answers at once once it runs: the deadline ends the wait
    // for the pixels, not for the server.
    const int wait = int(std::max<std::int64_t>(20, std::chrono::duration_cast<std::chrono::milliseconds>(full).count() / 3));
    ImageDecoder quick(AFTERGLOW_MUX, wait);
    // The first decode also waits for the server to start, which a loaded
    // machine can make slower than the deadline. A decode that fails stops
    // the server, and the next one starts a new one: up to five tries for a
    // server that answers by the deadline.
    bool warm = false;
    for (int attempt = 0; attempt < 5 && !warm; ++attempt) warm = decode(quick, Png, ImageFormats::Png).ok;
    ASSERT_TRUE(warm);
    start = Clock::now();
    EXPECT_FALSE(decode(quick, large, ImageFormats::Png).ok);
    EXPECT_GE(Clock::now() - start, std::chrono::milliseconds(wait));

    // The pane closes while its image decodes.
    const int cancel = ::eventfd(0, EFD_CLOEXEC);
    ASSERT_GE(cancel, 0);
    start = Clock::now();
    std::thread closes([cancel, wait] {
        std::this_thread::sleep_for(std::chrono::milliseconds(wait));
        const std::uint64_t one = 1;
        (void)!::write(cancel, &one, sizeof one);
    });
    EXPECT_FALSE(decode(decoder, large, ImageFormats::Png, cancel).ok);
    const auto took = Clock::now() - start;
    closes.join();
    EXPECT_GE(took, std::chrono::milliseconds(wait));
    // A pane that closed decodes nothing more, and the server goes on for
    // the others.
    EXPECT_FALSE(decode(decoder, Png, ImageFormats::Png, cancel).ok);
    ::close(cancel);
    EXPECT_TRUE(decode(decoder, Png, ImageFormats::Png).ok);
}

TEST(MuxImageDecoder, APaneThatClosesLeavesTheServerToTheOthers)
{
    // The real server, which answers a second late: the pane closes while
    // its request waits for the answer. exec keeps the process.
    TempDir dir;
    ASSERT_FALSE(dir.path.empty());
    const std::string slow = dir.script("slow", std::string("/bin/sleep 1\nexec '") + AFTERGLOW_MUX + "' \"$@\"");
    SKIP_WITHOUT(slow);
    if (!childrenListed()) GTEST_SKIP() << "the kernel lists no children of a process (CONFIG_PROC_CHILDREN)";
    const std::set<pid_t> before = children();
    ImageDecoder decoder(slow);
    const pid_t server = added(before, children());
    ASSERT_GT(server, 0);
    // A decode whose pane closes 100 ms after it began. False, and how long
    // it took.
    const auto closing = [&decoder](std::string_view image) {
        const int cancel = ::eventfd(0, EFD_CLOEXEC);
        if (cancel < 0) {
            ADD_FAILURE() << "eventfd";
            return Clock::duration::max();
        }
        const auto start = Clock::now();
        std::thread closes([cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const std::uint64_t one = 1;
            (void)!::write(cancel, &one, sizeof one);
        });
        EXPECT_FALSE(decode(decoder, image, ImageFormats::Png, cancel).ok);
        const auto took = Clock::now() - start;
        closes.join();
        ::close(cancel);
        return took;
    };
    // It ends before the answer comes, also when it waits for the answer
    // of the decode before it. The first pane's image is large: the server
    // forks a process for it after the pane closed.
    EXPECT_LT(closing(greyPng(8000, 8000)), std::chrono::milliseconds(900));
    EXPECT_LT(closing(Png), std::chrono::milliseconds(500));
    // The next decode takes the late answer of the first, and ends its
    // process: the large image does not decode on for nobody. The same
    // server decodes.
    EXPECT_TRUE(decode(decoder, Png, ImageFormats::Png).ok);
    EXPECT_EQ(added(before, children()), server);
    const auto taken = Clock::now();
    while (!children(server).empty() && Clock::now() - taken < std::chrono::seconds(5)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto gone = Clock::now() - taken;
    EXPECT_TRUE(children(server).empty()) << "a decoding process of a closed pane runs on";
    // A process that ran on would end with its decode, which began just
    // before: against the whole decode of that image, measured now with
    // the same server, it ends in far less.
    const std::string large = greyPng(8000, 8000);
    auto full = Clock::duration::max();
    for (int i = 0; i < 2; ++i) {
        const auto start = Clock::now();
        ASSERT_TRUE(decode(decoder, large, ImageFormats::Png).ok);
        full = std::min(full, Clock::now() - start);
    }
    EXPECT_LT(gone, full / 2) << "it took " << std::chrono::duration_cast<std::chrono::milliseconds>(gone).count() << " ms of a decode of "
                              << std::chrono::duration_cast<std::chrono::milliseconds>(full).count() << " ms";
}
