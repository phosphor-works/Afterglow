// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The image decoder of the daemon: the server afterglow-mux --decode-images,
// and a process that it forks for each PNG and iTerm2 image.

#include "ImageDecoder.h"
#include "ImageFiles.h"

#include <AfterglowProto/Zlib.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

using AfterglowMux::ImageDecoder;
using AfterglowMux::ImageFormats;
using namespace MuxTest::Images;
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

std::vector<std::uint8_t> repeat(std::vector<std::uint8_t> pixel, int count)
{
    std::vector<std::uint8_t> out;
    for (int i = 0; i < count; ++i) out.insert(out.end(), pixel.begin(), pixel.end());
    return out;
}

/// A grey PNG of @p width by @p height. stb_image does not check the CRCs.
std::string greyPng(std::uint32_t width, std::uint32_t height)
{
    const auto be = [](std::uint32_t v) {
        return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
    };
    const auto chunk = [&be](std::string_view type, const std::string& data) {
        return be(std::uint32_t(data.size())) + std::string(type) + data + std::string(4, '\0');
    };
    std::string rows;
    rows.reserve(std::size_t(height) * (width + 1));
    for (std::uint32_t y = 0; y < height; ++y) {
        rows += '\0';
        rows.append(width, char(y));
    }
    const std::string header = be(width) + be(height) + std::string{8, 0, 0, 0, 0};
    return std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", header) + chunk("IDAT", AfterglowProto::zlibCompress(rows))
        + chunk("IEND", {});
}

/// The processes that this process started: the servers of the decoders.
std::set<pid_t> children()
{
    std::set<pid_t> found;
    std::error_code ignored;
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task", ignored)) {
        std::ifstream list(task.path() / "children");
        for (pid_t pid = 0; list >> pid;) found.insert(pid);
    }
    return found;
}

/// A directory of its own, removed however the test ends.
struct TempDir
{
    std::filesystem::path path;
    TempDir()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "afterglow-mux-decoder-XXXXXX").string();
        if (::mkdtemp(pattern.data())) path = pattern;
    }
    ~TempDir()
    {
        std::error_code ignored;
        if (!path.empty()) std::filesystem::remove_all(path, ignored);
    }
    /// An executable script named @p name that runs @p body.
    std::string script(const std::string& name, const std::string& body) const
    {
        const std::string file = (path / name).string();
        std::ofstream(file) << "#!/bin/sh\n" << body << "\n";
        return ::chmod(file.c_str(), 0700) == 0 ? file : std::string();
    }
};

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

TEST(MuxImageDecoder, AnItermImageCanBeJpegGifOrBmp)
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
    for (const std::string* file : {&Jpeg, &Gif, &Bmp}) EXPECT_FALSE(decode(*file, ImageFormats::Png).ok);
    // WebP, which a pane outside a session shows: stb_image does not read it.
    EXPECT_FALSE(decode(Webp, ImageFormats::Common).ok);
    // Formats that stb_image reads and the window does not: they never
    // reach it.
    EXPECT_FALSE(decode(std::string("P6\n1 1\n255\n\xff\x00\x00", 14), ImageFormats::Common).ok);
    const std::string tga("\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x01\x00\x18\x00\x00\x00\xff", 21);
    EXPECT_FALSE(decode(tga, ImageFormats::Common).ok);
    EXPECT_FALSE(decode("", ImageFormats::Common).ok);
    EXPECT_FALSE(decode("not an image at all", ImageFormats::Common).ok);
    EXPECT_FALSE(decode(Png.substr(0, 40), ImageFormats::Png).ok);
    // A side past what a terminal takes: refused from the header, though
    // the file is whole.
    EXPECT_FALSE(decode(greyPng(10001, 1), ImageFormats::Png).ok);
    // Apple's CgBI: a chunk before IHDR.
    std::string cgbi = Png.substr(0, 8) + std::string("\x00\x00\x00\x04" "CgBI" "\x50\x00\x20\x06" "\x00\x00\x00\x00", 16) + Png.substr(8);
    EXPECT_FALSE(decode(cgbi, ImageFormats::Common).ok);
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
    ImageDecoder missing("/nonexistent/afterglow-mux");
    EXPECT_FALSE(decode(missing, Png, ImageFormats::Png).ok);
    // It ends at once: the decoder starts it again for the next image, once.
    ImageDecoder ends("/bin/false");
    EXPECT_FALSE(decode(ends, Png, ImageFormats::Png).ok);
    EXPECT_FALSE(decode(ends, Png, ImageFormats::Png).ok);
}

TEST(MuxImageDecoder, AServerThatDoesNotAnswerEndsAtTheDeadline)
{
    TempDir dir;
    ASSERT_FALSE(dir.path.empty());
    const std::string stuck = dir.script("stuck", "exec /bin/sleep 60");
    ASSERT_FALSE(stuck.empty());
    ImageDecoder decoder(stuck, 300);
    const auto start = Clock::now();
    EXPECT_FALSE(decode(decoder, Png, ImageFormats::Png).ok);
    const auto took = Clock::now() - start;
    // It waited for the server, which ran: a script that could not run
    // would fail at once.
    EXPECT_GE(took, std::chrono::milliseconds(300));
    EXPECT_LT(took, std::chrono::seconds(10));
}

TEST(MuxImageDecoder, ADecodeEndsAtItsDeadlineOrWhenItIsCancelled)
{
    // 8,000 by 8,000: far more than 300 ms of work, which it does decode.
    const std::string large = greyPng(8000, 8000);
    ImageDecoder decoder(AFTERGLOW_MUX);
    auto start = Clock::now();
    {
        const Decoded whole = decode(decoder, large, ImageFormats::Png);
        ASSERT_TRUE(whole.ok);
        EXPECT_EQ(whole.width, 8000u);
    }
    const auto full = Clock::now() - start;

    // The server answers at once once it runs: the deadline ends the wait
    // for the pixels, not for the server. A third of the whole decode, so
    // that a fast processor does not finish it.
    const int wait = int(std::max<std::int64_t>(20, std::chrono::duration_cast<std::chrono::milliseconds>(full).count() / 3));
    ImageDecoder quick(AFTERGLOW_MUX, wait);
    ASSERT_TRUE(decode(quick, Png, ImageFormats::Png).ok);
    start = Clock::now();
    EXPECT_FALSE(decode(quick, large, ImageFormats::Png).ok);
    EXPECT_GE(Clock::now() - start, std::chrono::milliseconds(wait));

    // The pane closes while its image decodes.
    const int cancel = ::eventfd(0, EFD_CLOEXEC);
    ASSERT_GE(cancel, 0);
    start = Clock::now();
    std::thread closes([cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::uint64_t one = 1;
        (void)!::write(cancel, &one, sizeof one);
    });
    EXPECT_FALSE(decode(decoder, large, ImageFormats::Png, cancel).ok);
    const auto took = Clock::now() - start;
    closes.join();
    EXPECT_GE(took, std::chrono::milliseconds(100));
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
    ASSERT_FALSE(slow.empty());
    if (!std::filesystem::exists("/proc/self/task/" + std::to_string(::gettid()) + "/children"))
        GTEST_SKIP() << "the kernel lists no children of a process (CONFIG_PROC_CHILDREN)";
    ImageDecoder decoder(slow);
    const std::set<pid_t> server = children();
    ASSERT_FALSE(server.empty());
    // A decode whose pane closes 100 ms after it began. False, and how long
    // it took.
    const auto closing = [&decoder] {
        const int cancel = ::eventfd(0, EFD_CLOEXEC);
        EXPECT_GE(cancel, 0);
        const auto start = Clock::now();
        std::thread closes([cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const std::uint64_t one = 1;
            (void)!::write(cancel, &one, sizeof one);
        });
        EXPECT_FALSE(decode(decoder, Png, ImageFormats::Png, cancel).ok);
        const auto took = Clock::now() - start;
        closes.join();
        ::close(cancel);
        return took;
    };
    // It ends before the answer comes, also when it waits for the answer
    // of the decode before it.
    EXPECT_LT(closing(), std::chrono::milliseconds(900));
    EXPECT_LT(closing(), std::chrono::milliseconds(500));
    // The next decode takes the late answers, and the same server decodes.
    EXPECT_TRUE(decode(decoder, Png, ImageFormats::Png).ok);
    EXPECT_EQ(children(), server);
}
