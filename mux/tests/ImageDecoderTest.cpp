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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using AfterglowMux::ImageDecoder;
using AfterglowMux::ImageFormats;
using namespace AfterglowImage::TestFiles;
using MuxTest::childrenListed;
using MuxTest::greyPng;
using MuxTest::TempDir;
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
ImageDecoder& shared()
{
    static ImageDecoder decoder(AFTERGLOW_MUX);
    return decoder;
}

Decoded decode(std::string_view data, ImageFormats formats)
{
    return decode(shared(), data, formats);
}

/// @p data as an iTerm2 image of a pane decodes (ImageFormats::CommonFrames,
/// decodeFrames): its first frame, which a still image has alone.
Decoded iterm(ImageDecoder& decoder, std::string_view data)
{
    Decoded d;
    std::vector<AfterglowMux::DecodedFrame> frames;
    d.ok = decoder.decodeFrames(data, ImageDecoder::MaxBytes, frames, -1) && !frames.empty();
    if (!d.ok)
        return d;
    d.width = frames[0].width;
    d.height = frames[0].height;
    d.rgba = std::move(frames[0].rgba);
    return d;
}

Decoded iterm(std::string_view data)
{
    return iterm(shared(), data);
}

/// The frames of @p data, up to @p maxBytes of pixels, with the shared
/// decoder: none when it decodes none.
std::vector<AfterglowMux::DecodedFrame> framesOf(std::string_view data, std::size_t maxBytes = std::size_t(1) << 20)
{
    std::vector<AfterglowMux::DecodedFrame> frames;
    if (!shared().decodeFrames(data, maxBytes, frames, -1))
        frames.clear();
    return frames;
}

/// The pixel at @p x, @p y of @p rgba, @p width wide: empty past its end.
std::vector<std::uint8_t> pixelAt(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t x,
                                  std::uint32_t y)
{
    const std::size_t at = (std::size_t(y) * width + x) * 4;
    if (at + 4 > rgba.size())
        return {};
    return std::vector<std::uint8_t>(rgba.begin() + std::ptrdiff_t(at), rgba.begin() + std::ptrdiff_t(at + 4));
}

/// A GIF of @p count frames, each the first frame of TestFiles::Gif: red,
/// 100 ms.
std::string gifOf(std::size_t count)
{
    const std::size_t first = Gif.find("\x21\xf9");
    const std::size_t second = Gif.find("\x21\xf9", first + 2);
    std::string out = Gif.substr(0, first);
    for (std::size_t i = 0; i < count; ++i)
        out += Gif.substr(first, second - first);
    return out + ';';
}

/// The processes that @p parent started: for this process, the servers of
/// the decoders, and for a server, its decoding processes.
std::set<pid_t> children(pid_t parent = ::getpid())
{
    std::set<pid_t> found;
    std::error_code ignored;
    for (const auto& task : std::filesystem::directory_iterator("/proc/" + std::to_string(parent) + "/task", ignored)) {
        std::ifstream list(task.path() / "children");
        for (pid_t pid = 0; list >> pid;)
            found.insert(pid);
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
        if (line.starts_with(prefix))
            out += line + '\n';
    }
    return out;
}

#define SKIP_WITHOUT(script)                                                                                           \
    do {                                                                                                               \
        if ((script).empty())                                                                                          \
            GTEST_SKIP() << "the temporary directory runs no program (noexec)";                                        \
    } while (false)

/// @p path as one word of sh, whatever its bytes: the scripts of these
/// tests name files under TMPDIR. The decoder runs a server with its own
/// arguments, so a script cannot take its paths as arguments.
std::string shellQuoted(const std::filesystem::path& path)
{
    std::string out = "'";
    for (const char c : path.string()) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

/// Whether a decode of this process runs: it holds the memory file of its
/// image (decode() makes it before it waits for the server).
bool decodeRuns()
{
    std::error_code ignored;
    for (const auto& fd : std::filesystem::directory_iterator("/proc/self/fd", ignored)) {
        std::error_code error;
        if (std::filesystem::read_symlink(fd.path(), error).string().starts_with("/memfd:afterglow-image"))
            return true;
    }
    return false;
}

} // namespace

TEST(MuxImageDecoder, APngDecodesToItsPixelsWithStraightAlpha)
{
    for (const bool kitty : {true, false}) {
        const Decoded d = kitty ? decode(Png, ImageFormats::Png) : iterm(Png);
        EXPECT_TRUE(d.ok) << kitty;
        if (!d.ok)
            continue;
        EXPECT_EQ(d.width, 3u);
        EXPECT_EQ(d.height, 2u);
        const std::vector<std::uint8_t> expected{255, 0,  0,  255, 0,   255, 0,  128, 0, 0, 255, 0,
                                                 10,  20, 30, 255, 200, 100, 50, 255, 1, 2, 3,   4};
        EXPECT_EQ(d.rgba, expected);
    }
}

TEST(MuxImageDecoder, AnItermImageCanBeOfEachFormatThatTheLibraryReads)
{
    Decoded d = iterm(Bmp);
    EXPECT_TRUE(d.ok);
    EXPECT_EQ(d.width, 2u);
    EXPECT_EQ(d.height, 2u);
    EXPECT_EQ(d.rgba, (std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}));
    // The first frame of an animation: AnimationsGiveEachFrameWithinTheBounds
    // has the others.
    for (const std::string* file : {&Gif, &WebpAnimated}) {
        d = iterm(*file);
        EXPECT_TRUE(d.ok);
        EXPECT_EQ(d.width, 4u);
        EXPECT_EQ(d.height, 4u);
        EXPECT_EQ(d.rgba, repeat({255, 0, 0, 255}, 16));
    }
    d = iterm(Apng);
    EXPECT_TRUE(d.ok);
    EXPECT_EQ(d.rgba, repeat({255, 0, 0, 255}, 16));
    // TIFF and netPBM, which the library reads itself.
    d = iterm(TiffRgb);
    EXPECT_TRUE(d.ok);
    EXPECT_EQ(d.width, 3u);
    EXPECT_EQ(d.height, 2u);
    EXPECT_EQ(d.rgba, (std::vector<std::uint8_t>{255, 0,  0,  255, 0,   255, 0,  255, 0, 0, 255, 255,
                                                 10,  20, 30, 255, 200, 100, 50, 255, 1, 2, 3,   255}));
    d = iterm(std::string("P6\n1 1\n255\n\xff\x00\x00", 14));
    EXPECT_TRUE(d.ok);
    EXPECT_EQ(d.rgba, (std::vector<std::uint8_t>{255, 0, 0, 255}));
    // An ICC profile and a cICP chunk convert to sRGB in the decoding
    // process too, under its system call filter.
    d = iterm(JpegAdobe);
    EXPECT_TRUE(d.ok);
    const std::vector<std::uint8_t> adobe = pixelAt(d.rgba, d.width, 0, 0);
    ASSERT_EQ(adobe.size(), 4u);
    EXPECT_NEAR(adobe[0], 66, 1);
    EXPECT_NEAR(adobe[1], 151, 1);
    EXPECT_NEAR(adobe[2], 202, 1);
    d = decode(PngCicpP3, ImageFormats::Png);
    EXPECT_TRUE(d.ok);
    ASSERT_EQ(d.width, 2u);
    ASSERT_EQ(d.height, 1u);
    const std::vector<std::uint8_t> p3 = pixelAt(d.rgba, d.width, 0, 0);
    const std::vector<std::uint8_t> green = pixelAt(d.rgba, d.width, 1, 0);
    ASSERT_EQ(p3.size(), 4u);
    ASSERT_EQ(green.size(), 4u);
    EXPECT_NEAR(p3[0], 83, 1);
    EXPECT_NEAR(p3[1], 152, 1);
    EXPECT_NEAR(p3[2], 205, 1);
    EXPECT_NEAR(green[0], 0, 1);
    EXPECT_NEAR(green[1], 204, 1);
    EXPECT_NEAR(green[2], 0, 1);
    d = iterm(Webp);
    EXPECT_TRUE(d.ok);
    EXPECT_EQ(d.width, 2u);
    EXPECT_EQ(d.height, 2u);
    EXPECT_EQ(d.rgba, repeat({255, 0, 0, 255}, 4));
    d = iterm(Jpeg);
    EXPECT_TRUE(d.ok);
    EXPECT_EQ(d.width, 8u);
    EXPECT_EQ(d.height, 8u);
    ASSERT_EQ(d.rgba.size(), 8u * 8 * 4);
    int notRed = 0;
    for (std::size_t i = 0; i < d.rgba.size(); i += 4) {
        if (d.rgba[i] <= 240 || d.rgba[i + 1] >= 16 || d.rgba[i + 2] >= 16 || d.rgba[i + 3] != 255)
            ++notRed;
    }
    EXPECT_EQ(notRed, 0);
}

// kitty's f=100 shows the default image of an APNG, as kitty does, also one
// that the animation does not show; iTerm2 shows the animation.
TEST(MuxImageDecoder, KittyShowsTheDefaultImageOfAnApng)
{
    Decoded d = decode(ApngHiddenDefault, ImageFormats::Png);
    EXPECT_TRUE(d.ok);
    EXPECT_EQ(d.width, 2u);
    EXPECT_EQ(d.height, 1u);
    EXPECT_EQ(d.rgba, (std::vector<std::uint8_t>{128, 128, 128, 255, 128, 128, 128, 255}));
    d = iterm(ApngHiddenDefault);
    EXPECT_TRUE(d.ok);
    EXPECT_EQ(d.rgba, (std::vector<std::uint8_t>{255, 255, 255, 255, 255, 255, 255, 255}));
    const std::vector<AfterglowMux::DecodedFrame> frames = framesOf(ApngHiddenDefault);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[1].rgba, (std::vector<std::uint8_t>{0, 0, 0, 255, 0, 0, 0, 255}));
}

// Each frame for iTerm2's animations, with its time, as the panes of a
// window and of a session take them.
TEST(MuxImageDecoder, AnimationsGiveEachFrameWithinTheBounds)
{
    for (const std::string* file : {&Gif, &WebpAnimated}) {
        SCOPED_TRACE(file == &Gif ? "GIF" : "WebP");
        std::vector<AfterglowMux::DecodedFrame> frames = framesOf(*file);
        if (frames.size() != 2) {
            ADD_FAILURE() << frames.size() << " frames";
            continue;
        }
        EXPECT_EQ(frames[0].delayMs, 100u);
        EXPECT_EQ(frames[1].delayMs, 100u);
        EXPECT_EQ(frames[0].rgba, repeat({255, 0, 0, 255}, 16));
        EXPECT_EQ(frames[1].rgba, repeat({0, 0, 255, 255}, 16));
        // A frame of 4 by 4 is 64 bytes: 100 hold the first, not the
        // second, and a single frame shows with no time.
        frames = framesOf(*file, 100);
        if (frames.size() != 1) {
            ADD_FAILURE() << frames.size() << " frames within 100 bytes";
            continue;
        }
        EXPECT_EQ(frames[0].delayMs, 0u);
    }
    // An APNG: its frames on the canvas, with their times.
    const std::vector<AfterglowMux::DecodedFrame> apng = framesOf(Apng);
    ASSERT_EQ(apng.size(), 3u);
    EXPECT_EQ(apng[0].delayMs, 100u);
    EXPECT_EQ(apng[1].delayMs, 50u);
    EXPECT_EQ(apng[2].delayMs, 30u);
    EXPECT_EQ(apng[0].rgba, repeat({255, 0, 0, 255}, 16));
    // A still image: one frame, with no time.
    const std::vector<AfterglowMux::DecodedFrame> still = framesOf(Png);
    ASSERT_EQ(still.size(), 1u);
    EXPECT_EQ(still[0].delayMs, 0u);
    // An animation that the decoder cannot finish keeps the frames before
    // the fault: the third frame of this APNG is out of sequence.
    EXPECT_EQ(framesOf(ApngBadSequence).size(), 2u);
    // At most GraphicsMaxFrames frames: the rest go.
    const std::vector<AfterglowMux::DecodedFrame> many = framesOf(gifOf(AfterglowProto::GraphicsMaxFrames + 5));
    EXPECT_EQ(many.size(), AfterglowProto::GraphicsMaxFrames);
}

TEST(MuxImageDecoder, RefusesOtherFormatsAndBadFiles)
{
    // kitty's f=100 is PNG only.
    const std::string ppm("P6\n1 1\n255\n\xff\x00\x00", 14);
    for (const std::string* file : {&Jpeg, &Gif, &Bmp, &Webp, &TiffRgb, &ppm})
        EXPECT_FALSE(decode(*file, ImageFormats::Png).ok);
    EXPECT_TRUE(iterm(ppm).ok);
    // A format that no decoder here reads (TGA): its signature is unknown.
    // Corrupt files of the formats that it reads are the library's tests
    // (Image.ImageDecode.*): there a sanitizer's report fails the test,
    // where here it ends a decoding process as a refusal does.
    const std::string tga("\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x01\x00\x18\x00\x00\x00\xff", 21);
    EXPECT_FALSE(iterm(tga).ok);
    EXPECT_FALSE(iterm("").ok);
    EXPECT_FALSE(iterm("not an image at all").ok);
    EXPECT_FALSE(decode(Png.substr(0, 40), ImageFormats::Png).ok);
    // A side past what a terminal takes: refused from the header, though
    // the file is whole.
    EXPECT_FALSE(decode(greyPng(10001, 1), ImageFormats::Png).ok);
    // Pixels past MaxBytes, with each side within it: 10,000 by 8,389 is
    // 335,560,000 bytes, 15,680 more than the 320 MiB.
    EXPECT_FALSE(decode(greyPng(10000, 8389), ImageFormats::Png).ok);
    // Apple's CgBI: a chunk before IHDR.
    EXPECT_FALSE(iterm(cgbi()).ok);
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
        const std::string later = dir.script("later", "echo started >> " + shellQuoted(starts) + "\nexit 1");
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
    const std::string ends = dir.script("ends", "echo started >> " + shellQuoted(starts) + "\nexit 1");
    SKIP_WITHOUT(ends);
    ImageDecoder decoder(ends);
    for (int i = 0; i < 3; ++i)
        EXPECT_FALSE(decode(decoder, Png, ImageFormats::Png).ok);
    std::ifstream log(starts);
    int count = 0;
    for (std::string line; std::getline(log, line);)
        ++count;
    EXPECT_EQ(count, 2);
}

TEST(MuxImageDecoder, AServerKilledBeforeItsFirstImageStartsAgain)
{
    // The server ran, and the OOM killer or a user ended it before any
    // image came: it did start, so the next decode starts another.
    if (!childrenListed())
        GTEST_SKIP() << "the kernel lists no children of a process (CONFIG_PROC_CHILDREN)";
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
    if (!childrenListed())
        GTEST_SKIP() << "the kernel lists no children of a process (CONFIG_PROC_CHILDREN)";
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
    const std::string stuck =
        dir.script("stuck",
                   "if [ ! -e " + shellQuoted(once) + " ]; then : > " + shellQuoted(once)
                       + "; exec /bin/sleep 60; fi\nexec " + shellQuoted(AFTERGLOW_MUX) + " \"$@\"");
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
    for (int attempt = 0; attempt < 5 && !decoded; ++attempt)
        decoded = decode(decoder, Png, ImageFormats::Png).ok;
    EXPECT_TRUE(decoded);
}

TEST(MuxImageDecoder, ADecodeThatWaitsForAnotherEndsWhenItsPaneCloses)
{
    // One pane's request waits 1 s for a server that does not answer. A
    // second pane waits behind it, and closes: its decode ends at once, not
    // when its own wait ends, 1 s after it began.
    TempDir dir;
    ASSERT_FALSE(dir.path.empty());
    const std::string stuck = dir.script("stuck", "exec /bin/sleep 60");
    SKIP_WITHOUT(stuck);
    ASSERT_FALSE(decodeRuns());
    ImageDecoder decoder(stuck, 1000);
    // Joined also when an ASSERT below returns.
    std::jthread first([&decoder] {
        EXPECT_FALSE(decode(decoder, Png, ImageFormats::Png).ok);
    });
    // The first request holds the server once its image is in a memory
    // file: it takes the server right after, before the second one makes its
    // own.
    ASSERT_TRUE(MuxTest::waitFor(decodeRuns));
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
    // The close comes at 100 ms, and the wait ends at 1000 ms.
    EXPECT_LT(took, std::chrono::milliseconds(500));
}

TEST(MuxImageDecoder, TheServerTakesOnlyItsClientOfItsVersion)
{
    // aglow-mux --decode-images runs only as the child of the client whose
    // process id it gets, and of the version of its wire: another version
    // ends at once with its status, which the client reads.
    const auto server = [](const std::string& parent, std::optional<std::string> version) {
        pid_t pid = 0;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        const std::string argument(AfterglowMux::DecodeServerArgument);
        std::string parentArgument = parent;
        std::vector<char*> argv{const_cast<char*>(AFTERGLOW_MUX), const_cast<char*>(argument.c_str()),
                                parentArgument.data()};
        if (version)
            argv.push_back(version->data());
        argv.push_back(nullptr);
        char* const env[] = {nullptr};
        const int spawned = ::posix_spawn(&pid, AFTERGLOW_MUX, &actions, nullptr, argv.data(), env);
        posix_spawn_file_actions_destroy(&actions);
        int status = -1;
        if (spawned != 0 || ::waitpid(pid, &status, 0) != pid)
            return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    };
    const std::string self = std::to_string(::getpid());
    EXPECT_EQ(server(std::to_string(::getppid()), std::string(AfterglowMux::DecodeWireVersion)), 1);
    EXPECT_EQ(server(self, std::string("0")), 64);
    EXPECT_EQ(server(self, std::nullopt), 64);
}

TEST(MuxImageDecoder, TheServerStartsNoProcessForFormatsOfNoVersion)
{
    // A request byte that names no ImageFormats gets the answer with no
    // process, and the server goes on.
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair), 0);
    AfterglowProto::UniqueFd client(pair[0]);
    AfterglowProto::UniqueFd serverEnd(pair[1]);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, serverEnd.get(), STDIN_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    std::string argument(AfterglowMux::DecodeServerArgument);
    std::string parent = std::to_string(::getpid());
    std::string version(AfterglowMux::DecodeWireVersion);
    char* argv[] = {const_cast<char*>(AFTERGLOW_MUX), argument.data(), parent.data(), version.data(), nullptr};
    char* const env[] = {nullptr};
    pid_t pid = 0;
    const int spawned = ::posix_spawn(&pid, AFTERGLOW_MUX, &actions, nullptr, argv, env);
    posix_spawn_file_actions_destroy(&actions);
    ASSERT_EQ(spawned, 0);
    serverEnd.reset();

    const auto request = [&client](std::uint8_t formats) -> std::optional<std::uint8_t> {
        // An empty image: a process refuses it at once.
        AfterglowProto::UniqueFd reader(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        AfterglowProto::UniqueFd writer(::open("/dev/null", O_WRONLY | O_CLOEXEC));
        if (!reader.valid() || !writer.valid())
            return std::nullopt;
        iovec io{&formats, 1};
        alignas(cmsghdr) char control[CMSG_SPACE(2 * sizeof(int))] = {};
        msghdr message{};
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof control;
        cmsghdr* c = CMSG_FIRSTHDR(&message);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(2 * sizeof(int));
        const int fds[2] = {reader.get(), writer.get()};
        std::memcpy(CMSG_DATA(c), fds, sizeof fds);
        if (::sendmsg(client.get(), &message, MSG_NOSIGNAL) != 1)
            return std::nullopt;
        std::uint8_t answer = 0xff;
        alignas(cmsghdr) char received[CMSG_SPACE(sizeof(int))] = {};
        iovec back{&answer, 1};
        msghdr reply{};
        reply.msg_iov = &back;
        reply.msg_iovlen = 1;
        reply.msg_control = received;
        reply.msg_controllen = sizeof received;
        if (::recvmsg(client.get(), &reply, MSG_CMSG_CLOEXEC) != 1)
            return std::nullopt;
        for (cmsghdr* r = CMSG_FIRSTHDR(&reply); r; r = CMSG_NXTHDR(&reply, r)) {
            int pidfd = -1;
            std::memcpy(&pidfd, CMSG_DATA(r), sizeof pidfd);
            ::close(pidfd);
        }
        return answer;
    };
    EXPECT_EQ(request(0x7f), std::optional<std::uint8_t>(0));
    // The server still answers: a request of a known format gets its process.
    EXPECT_EQ(request(std::uint8_t(ImageFormats::Png)), std::optional<std::uint8_t>(1));
    client.reset();
    int status = -1;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
}

TEST(MuxImageDecoder, AServerOfAnotherVersionStartsNoMore)
{
    // A client hears why, and starts that executable no more. A server that
    // says so (64), and an aglow-mux from before the version argument, whose
    // usage error (2) refuses the arguments of this one.
    for (const char* status : {"64", "2"}) {
        TempDir dir;
        ASSERT_FALSE(dir.path.empty());
        const std::string starts = (dir.path / "starts").string();
        const std::string other = dir.script("other", "echo started >> " + shellQuoted(starts) + "\nexit " + status);
        SKIP_WITHOUT(other);
        std::vector<std::string> reports;
        std::mutex mutex;
        ImageDecoder decoder(other, ImageDecoder::WaitMs, [&](const std::string& line) {
            std::lock_guard lock(mutex);
            reports.push_back(line);
        });
        for (int i = 0; i < 3; ++i)
            EXPECT_FALSE(decode(decoder, Png, ImageFormats::Png).ok) << status;
        std::ifstream log(starts);
        int count = 0;
        for (std::string line; std::getline(log, line);)
            ++count;
        EXPECT_EQ(count, 1) << status;
        std::lock_guard lock(mutex);
        EXPECT_TRUE(std::any_of(reports.begin(), reports.end(),
                                [](const std::string& line) {
                                    return line.find("of another version") != std::string::npos;
                                }))
            << status << ' ' << testing::PrintToString(reports);
    }
}

TEST(MuxImageDecoder, ADecodingProcessHasItsLimitsAndItsFilter)
{
    // While a large image decodes, its process: no new privileges, the
    // filter of its system calls, and limits on its memory, its CPU time
    // and its core file.
    if (!childrenListed())
        GTEST_SKIP() << "the kernel lists no children of a process (CONFIG_PROC_CHILDREN)";
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
                const std::string seen =
                    procLines(process, "status", "Seccomp:") + procLines(process, "status", "NoNewPrivs:");
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
    // The process can end between its status and its limits: then there is
    // nothing to check, which is a failure, not a pass.
    ASSERT_FALSE(limits.empty()) << "the decoding process ended before its limits were read";
    const auto line = [&limits](const char* name) {
        const std::size_t at = limits.find(name);
        return at == std::string::npos ? std::string() : limits.substr(at, limits.find('\n', at) - at);
    };
    for (const char* name : {"Max address space", "Max cpu time", "Max core file size"}) {
        EXPECT_FALSE(line(name).empty()) << name << ": " << limits;
    }
    EXPECT_EQ(line("Max address space").find("unlimited"), std::string::npos) << limits;
    EXPECT_EQ(line("Max cpu time").find("unlimited"), std::string::npos) << limits;
    EXPECT_NE(line("Max core file size").find(" 0 "), std::string::npos) << limits;
}

TEST(MuxImageDecoder, PanesDecodeAtOnce)
{
    // The PTY reader threads of the panes call decode() at once: each gets
    // the pixels of its own image, as one decode alone gives them.
    ImageDecoder decoder(AFTERGLOW_MUX);
    const std::string* const files[] = {&Png, &Gif, &Bmp};
    std::vector<Decoded> alone;
    for (const std::string* file : files) {
        alone.push_back(iterm(decoder, *file));
        ASSERT_TRUE(alone.back().ok);
    }
    std::vector<std::thread> panes;
    std::atomic<int> wrong = 0;
    for (int pane = 0; pane < 4; ++pane) {
        panes.emplace_back([&decoder, &wrong, &files, &alone, pane] {
            for (int i = 0; i < 20; ++i) {
                const std::size_t which = std::size_t(pane + i) % 3;
                const Decoded d = iterm(decoder, *files[which]);
                const Decoded& expected = alone[which];
                if (!d.ok || d.width != expected.width || d.height != expected.height || d.rgba != expected.rgba)
                    ++wrong;
            }
        });
    }
    for (auto& pane : panes)
        pane.join();
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
    const int wait =
        int(std::max<std::int64_t>(20, std::chrono::duration_cast<std::chrono::milliseconds>(full).count() / 3));
    ImageDecoder quick(AFTERGLOW_MUX, wait);
    // The first decode also waits for the server to start, which a loaded
    // machine can make slower than the deadline. A decode that fails stops
    // the server, and the next one starts a new one: up to five tries for a
    // server that answers by the deadline.
    bool warm = false;
    for (int attempt = 0; attempt < 5 && !warm; ++attempt)
        warm = decode(quick, Png, ImageFormats::Png).ok;
    ASSERT_TRUE(warm);
    // Each wait ends between its deadline and the end of a whole decode:
    // halfway between them is the bound.
    const auto bound = std::chrono::milliseconds(wait) + (full - std::chrono::milliseconds(wait)) / 2;
    start = Clock::now();
    EXPECT_FALSE(decode(quick, large, ImageFormats::Png).ok);
    const auto waited = Clock::now() - start;
    EXPECT_GE(waited, std::chrono::milliseconds(wait));
    EXPECT_LT(waited, bound) << std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()
                             << " ms of a decode of "
                             << std::chrono::duration_cast<std::chrono::milliseconds>(full).count() << " ms";

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
    EXPECT_LT(took, bound) << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
                           << " ms of a decode of "
                           << std::chrono::duration_cast<std::chrono::milliseconds>(full).count() << " ms";
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
    const std::string slow = dir.script("slow", "/bin/sleep 1\nexec " + shellQuoted(AFTERGLOW_MUX) + " \"$@\"");
    SKIP_WITHOUT(slow);
    if (!childrenListed())
        GTEST_SKIP() << "the kernel lists no children of a process (CONFIG_PROC_CHILDREN)";
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
    while (!children(server).empty() && Clock::now() - taken < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
    EXPECT_LT(gone, full / 2) << "it took " << std::chrono::duration_cast<std::chrono::milliseconds>(gone).count()
                              << " ms of a decode of "
                              << std::chrono::duration_cast<std::chrono::milliseconds>(full).count() << " ms";
}
