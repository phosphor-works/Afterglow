// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"
#include "../src/ITerm2.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Graphics.h>
#include <AfterglowProto/Placeholder.h>
#include <AfterglowProto/Zlib.h>

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;
using AfterglowProto::base64Encode;
using AfterglowProto::zlibCompress;

namespace {

std::string rgba(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
{
    std::string px;
    for (int i = 0; i < w * h; ++i) {
        px += char(r);
        px += char(g);
        px += char(b);
        px += char(a);
    }
    return px;
}

std::string utf8(char32_t cp)
{
    std::string out;
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
    return out;
}

/// A placeholder cell with the diacritics given; a negative value writes
/// U+0301, a combining mark that is not one of them.
std::string placeholder(std::initializer_list<int> values)
{
    std::string out = utf8(AfterglowProto::PlaceholderChar);
    for (const int v : values) {
        out += utf8(v < 0 ? char32_t(0x0301) : AfterglowProto::PlaceholderDiacritics[v]);
    }
    return out;
}

std::string apc(std::string_view keys, std::string_view payload = {})
{
    std::string s = "\x1b_G";
    s += keys;
    if (!payload.empty()) {
        s += ';';
        s += payload;
    }
    s += "\x1b\\";
    return s;
}

struct Term
{
    RecordingHost host;
    Terminal t;
    Term(int cols, int rows, const TerminalOptions& options = {})
        : t(cols, rows, host, options)
    {
        t.resize(cols, rows, 10, 20); // give the terminal a cell size
    }
    const GraphicsStore& g() const { return t.graphics(); }
};

/// A store driven directly, for what Terminal does not route yet or what a
/// test needs to set (file transfer, the cell size of a clear).
struct Store
{
    RecordingHost host;
    Screen screen;
    GraphicsStore g;
    Store(int cols, int rows, std::size_t scrollback = 100)
        : screen(cols, rows, scrollback)
    {
    }
    void apc(std::string_view keys, std::string_view payload = {})
    {
        std::string p(keys);
        if (!payload.empty()) {
            p += ';';
            p += payload;
        }
        GraphicsStore::Context ctx{screen, host, 10, 20, [this](std::string_view bytes) { host.written.append(bytes); },
                                   [this] {
                                       if (screen.cursor.y + 1 < screen.rows()) ++screen.cursor.y;
                                       else screen.scrollUp(Region{0, screen.rows() - 1, 0, screen.cols() - 1}, 1,
                                                            StyleTable::DefaultId, true);
                                   }};
        g.handle(p, ctx);
    }
};

/// Removes a POSIX shared-memory object when the test ends, however it ends.
struct ShmName
{
    std::string name;
    ~ShmName() { shm_unlink(name.c_str()); }
};

/// A file in the test's temporary directory, removed when the test ends.
/// mkstemp picks a name nobody could have planted a link at; the stem keeps
/// the protocol's tag in it.
struct TempFile
{
    std::string path;
    explicit TempFile(std::string_view stem)
        : path((std::filesystem::temp_directory_path() / (std::string(stem) + "XXXXXX")).string())
    {
        const int fd = mkstemp(path.data());
        if (fd >= 0) close(fd);
    }
    ~TempFile() { unlink(path.c_str()); }
    /// (Re)writes the file; a link put in its place is not followed.
    bool write(std::string_view bytes) const
    {
        const int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return false;
        const bool ok = ::write(fd, bytes.data(), bytes.size()) == ssize_t(bytes.size());
        close(fd);
        return ok;
    }
};

} // namespace

TEST(Graphics, TransmitAndRespond)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=2,v=2,i=7", base64Encode(rgba(2, 2, 255, 0, 0))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=7;OK\x1b\\");
    ASSERT_EQ(t.g().images().count(7), 1u);
    EXPECT_EQ(t.g().images().at(7).width, 2u);
    EXPECT_EQ(t.g().images().at(7).frames[0]->rgba[0], 255);
}

TEST(Graphics, QuietAndAnonymous)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=1", base64Encode(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=t,f=32,s=1,v=1", base64Encode(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=t,f=32,s=9,v=9,i=2,q=1", base64Encode(rgba(1, 1, 1, 2, 3))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;ENODATA:insufficient image data\x1b\\");
    // The anonymous image is not kept: no command can name or show it.
    EXPECT_EQ(t.g().images().size(), 1u);
    EXPECT_EQ(t.g().images().count(1), 1u);
}

TEST(Graphics, ImageNumberGetsAnId)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,I=13", base64Encode("abc")));
    ASSERT_EQ(t.g().images().size(), 1u);
    const std::uint32_t id = t.g().images().begin()->first;
    EXPECT_EQ(t.host.written, "\x1b_Gi=" + std::to_string(id) + ",I=13;OK\x1b\\");
}

TEST(Graphics, ChunkedTransmission)
{
    Term t(20, 10);
    const std::string data = base64Encode(rgba(4, 4, 9, 8, 7));
    t.t.feed(apc("a=t,f=32,s=4,v=4,i=3,m=1", data.substr(0, 20)));
    t.t.feed(apc("m=1", data.substr(20, 40)));
    t.t.feed(apc("m=0", data.substr(60)));
    ASSERT_EQ(t.g().images().count(3), 1u);
    EXPECT_EQ(t.g().images().at(3).frames[0]->rgba.size(), 64u);
    EXPECT_EQ(t.host.written, "\x1b_Gi=3;OK\x1b\\");
}

TEST(Graphics, ChunksPastTheImageStillCountForThePayloadLimit)
{
    // A 1x1 image keeps 4 bytes of its chunks, but a transmission longer
    // than the payload limit still fails.
    Term t(20, 10);
    const std::string chunk = base64Encode(std::string(3072, '\x01'));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,m=1", chunk));
    for (std::size_t sent = 3072; sent <= GraphicsStore::DefaultQuota; sent += 3072) {
        t.t.feed(apc("m=1", chunk));
    }
    t.t.feed(apc("m=0"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;EFBIG:image payload too large\x1b\\");
    EXPECT_TRUE(t.g().images().empty());
    // One that ends at the limit exactly is kept. Its chunks of 6 MiB each
    // go far past the image.
    t.host.written.clear();
    const std::size_t large = std::size_t(6) << 20;
    const std::string big = base64Encode(std::string(large, '\x01'));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=2,m=1", big));
    std::size_t sent = large;
    for (; sent + large <= GraphicsStore::DefaultQuota; sent += large) {
        t.t.feed(apc("m=1", big));
    }
    t.t.feed(apc("m=0", base64Encode(std::string(GraphicsStore::DefaultQuota - sent, '\x01'))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;OK\x1b\\");
}

TEST(Graphics, ZlibCompressed)
{
    Term t(20, 10);
    const std::string raw = rgba(8, 8, 1, 2, 3);
    t.t.feed(apc("a=t,f=32,s=8,v=8,o=z,i=4", base64Encode(zlibCompress(raw))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=4;OK\x1b\\");
    ASSERT_EQ(t.g().images().count(4), 1u);
    const auto& pixels = t.g().images().at(4).frames[0]->rgba;
    EXPECT_EQ(std::string(pixels.begin(), pixels.end()), raw);
}

TEST(Graphics, ZlibBombStopsAtTheDeclaredSize)
{
    // 4 MiB of zeros, far under the quota, compress to a few KiB. Declared
    // 1x1, they may inflate to 4 bytes: the first step past that refuses them.
    const std::string z = zlibCompress(std::string(4u << 20, '\0'));
    ASSERT_FALSE(z.empty());
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,o=z,i=4", base64Encode(z)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=4;EINVAL:zlib decompression failed\x1b\\");
    EXPECT_TRUE(t.g().images().empty());
}

TEST(Graphics, ZlibDataPastTheDeclaredSizeIsRefused)
{
    Term t(20, 10);
    // RGB 1x1 is 3 bytes: a stream of exactly that is stored, one byte more is not.
    t.t.feed(apc("a=t,f=24,s=1,v=1,o=z,i=1", base64Encode(zlibCompress("abc"))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;OK\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=t,f=24,s=1,v=1,o=z,i=2", base64Encode(zlibCompress("abcd"))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;EINVAL:zlib decompression failed\x1b\\");
    t.host.written.clear();
    // A size missing or past the quota is refused before anything inflates:
    // data that is not zlib at all gets the size's error, not the stream's.
    t.t.feed(apc("a=t,f=32,v=1,o=z,i=3", base64Encode("not zlib")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=3;EINVAL:missing or invalid image size\x1b\\");
    EXPECT_EQ(t.g().images().count(1), 1u);
    EXPECT_EQ(t.g().images().count(2), 0u);
    TerminalOptions small;
    small.graphicsQuota = 4096;
    Term q(20, 10, small);
    q.t.feed(apc("a=t,f=32,s=40,v=40,o=z,i=5", base64Encode("not zlib"))); // 6,400 bytes
    EXPECT_EQ(q.host.written, "\x1b_Gi=5;ENOSPC:image larger than the storage quota\x1b\\");
    EXPECT_TRUE(q.g().images().empty());
}

TEST(Graphics, UnknownCompressionIsRefused)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,o=x,i=1", base64Encode(rgba(1, 1, 1, 2, 3))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;EINVAL:unknown compression\x1b\\");
    EXPECT_TRUE(t.g().images().empty());
}

TEST(Graphics, PlaceMovesCursorAndScrollsWithText)
{
    Term t(20, 10);
    // 25x45 px image at 10x20 cells: 3 cells wide, 3 rows high.
    t.t.feed("\x1b[2;3H");
    t.t.feed(apc("a=T,f=32,s=25,v=45,i=5", base64Encode(rgba(25, 45, 0, 0, 255))));
    EXPECT_EQ(t.t.screen().cursor.y, 3);
    EXPECT_EQ(t.t.screen().cursor.x, 5);
    auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].x, 20);
    EXPECT_EQ(vis[0].y, 20);
    // scroll three lines: the image moves up with the text
    t.t.feed("\x1b[10;1H\n\n\n");
    vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, -40);
}

TEST(Graphics, PlacementSizeAndNoCursorMove)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=10,v=10,i=6", base64Encode(rgba(10, 10, 0, 0, 0))));
    t.t.feed(apc("a=p,i=6,c=4,C=1"));
    EXPECT_EQ(t.t.screen().cursor.x, 0);
    auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].width, 40);
    EXPECT_EQ(vis[0].height, 40);
}

TEST(Graphics, DeleteByIdAndFree)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=8,q=2", base64Encode(rgba(1, 1, 0, 0, 0))));
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed(apc("a=d,d=i,i=8"));
    EXPECT_TRUE(t.g().placements().empty());
    EXPECT_EQ(t.g().images().count(8), 1u); // lowercase keeps the image
    t.t.feed(apc("a=d,d=I,i=8"));
    EXPECT_EQ(t.g().images().count(8), 0u);
}

TEST(Graphics, ClearScreenDropsPlacements)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=9,q=2", base64Encode(rgba(1, 1, 0, 0, 0))));
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed("\x1b[2J");
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, ClearScreenDropsImagesThatReachItFromTheScrollback)
{
    Term t(20, 5);
    t.t.feed("\x1b[5;1H");
    // Three rows high (cells are 20 px) from the bottom row, then three more
    // line feeds: its top row is history, its last row still on the screen.
    t.t.feed(apc("a=T,f=32,s=10,v=60,i=1,q=2", base64Encode(rgba(10, 60, 0, 0, 0))));
    t.t.feed("\n\n\n");
    ASSERT_EQ(t.t.screen().historySize(), 5u);
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed("\x1b[2J");
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, EraseLineDropsAnImageOnThatLine)
{
    Term t(20, 10);
    // A 25x45 image at 10x20 cells covers three rows from the top.
    t.t.feed(apc("a=T,f=32,s=25,v=45,i=5", base64Encode(rgba(25, 45, 0, 0, 255))));
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed("\x1b[H\x1b[K");
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, EraseToEndOfScreenDropsImagesBelowTheCursor)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,f=32,s=25,v=45,i=5", base64Encode(rgba(25, 45, 0, 0, 255))));
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed("\x1b[H\x1b[J");
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, EraseFromTopDropsImagesAboveTheCursor)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,f=32,s=25,v=45,i=5", base64Encode(rgba(25, 45, 0, 0, 255))));
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed("\x1b[4;1H\x1b[1J");
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, EraseOfANonOverlappingRowKeepsTheImage)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,f=32,s=25,v=45,i=5", base64Encode(rgba(25, 45, 0, 0, 255))));
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed("\x1b[6;1H\x1b[K"); // the image ends on row 2; row 5 is below it
    EXPECT_EQ(t.g().placements().size(), 1u);
}

TEST(Graphics, SixelAllocationAndCoveragePreserveKittyImagesInInternalRange)
{
    Term t(20, 10);
    const auto id = 3u << 29;
    t.t.feed(apc("a=T,f=32,s=1,v=1,C=1,q=2,i=" + std::to_string(id), base64Encode(rgba(1, 1, 255, 0, 0))));
    t.t.feed("\x1bPq#1~\x1b\\");
    ASSERT_EQ(t.g().placements().size(), 2u);
    EXPECT_FALSE(t.g().images().at(id).isSixel);
    EXPECT_EQ(t.g().images().at(id).frames[0]->rgba[0], 255);
    const auto firstSixel = t.g().placements().back().imageId;
    EXPECT_NE(firstSixel, id);
    EXPECT_TRUE(t.g().images().at(firstSixel).isSixel);
    t.t.feed("\x1b[H\x1bPq#2~\x1b\\");
    EXPECT_EQ(t.g().placements().size(), 2u); // only the covered sixel is replaced
    EXPECT_EQ(t.g().images().count(firstSixel), 0u);
    EXPECT_EQ(t.g().images().count(id), 1u);
}

TEST(Graphics, KittyClaimRelocatesExistingInternalImageAndPreservesItsAnchor)
{
    Term t(20, 10);
    t.t.feed("\x1b[2;3H\x1bPq#1~\x1b\\");
    ASSERT_EQ(t.g().placements().size(), 1u);
    const auto old = t.g().placements().front();
    const auto oldFrame = t.g().images().at(old.imageId).frames[0];
    t.t.feed("\x1b[5;6H");
    t.t.feed(apc("a=T,f=32,s=1,v=1,C=1,q=2,i=" + std::to_string(old.imageId), base64Encode(rgba(1, 1, 255, 0, 0))));
    ASSERT_EQ(t.g().placements().size(), 2u);
    const auto relocated = t.g().placements().front();
    EXPECT_NE(relocated.imageId, old.imageId);
    EXPECT_EQ(relocated.row, old.row);
    EXPECT_EQ(relocated.col, old.col);
    EXPECT_EQ(t.g().images().at(relocated.imageId).frames[0], oldFrame);
    EXPECT_FALSE(t.g().images().at(old.imageId).isSixel);
    t.t.feed("\x1b[2;3H\x1bPq#2~\x1b\\");
    EXPECT_EQ(t.g().images().count(relocated.imageId), 0u);
    EXPECT_EQ(t.g().images().count(old.imageId), 1u);
    EXPECT_EQ(t.g().placements().size(), 2u);
}

TEST(Graphics, KittyCommandsCannotAddressAnInternalImage)
{
    Term t(20, 10);
    t.t.feed("\x1bPq#1~\x1b\\");
    ASSERT_EQ(t.g().placements().size(), 1u);
    const auto id = t.g().placements().front().imageId;
    const auto identity = std::to_string(id);
    t.host.written.clear();
    t.t.feed(apc("a=p,i=" + identity));
    EXPECT_NE(t.host.written.find("ENOENT"), std::string::npos);
    t.t.feed(apc("a=d,d=I,i=" + identity));
    t.t.feed(apc("a=d,d=R,x=" + identity + ",y=" + identity));
    EXPECT_EQ(t.g().images().count(id), 1u);
    EXPECT_EQ(t.g().placements().size(), 1u);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 1, 2, 3))));
    t.host.written.clear();
    t.t.feed(apc("a=p,i=1,P=" + identity));
    EXPECT_NE(t.host.written.find("ENOPARENT"), std::string::npos);
    EXPECT_EQ(t.g().placements().size(), 1u);
}

TEST(Graphics, AltScreenHasItsOwnImages)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 0, 0, 0))));
    t.t.feed("\x1b[?1049h");
    EXPECT_TRUE(t.g().images().empty());
    t.t.feed("\x1b[?1049l");
    EXPECT_EQ(t.g().images().size(), 1u);
}

TEST(Graphics, AlternateScreenClearRemovesPlacementsAcrossSessions)
{
    Term t(20, 10);
    t.t.feed("\x1b[?47h");
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=1,C=1,q=2", base64Encode(rgba(1, 1, 1, 2, 3))));
    t.t.feed("\x1b[?47l\x1b[?47h");
    ASSERT_EQ(t.g().placements().size(), 1u); // mode 47 preserves the screen
    t.t.feed("\x1b[?47l\x1b[?1049h");
    EXPECT_TRUE(t.g().placements().empty()); // 1049 clears on entry
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=2,C=1,q=2", base64Encode(rgba(1, 1, 4, 5, 6))));
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed("\x1b[?1049l\x1b[?47h");
    EXPECT_TRUE(t.g().placements().empty()); // and on exit
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=3,C=1,q=2", base64Encode(rgba(1, 1, 7, 8, 9))));
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed("\x1b[?1047l\x1b[?47h");
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, FramesAndAnimation)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=2,v=1,i=10,q=2", base64Encode(rgba(2, 1, 255, 0, 0))));
    t.t.feed(apc("a=f,f=32,s=1,v=1,i=10,x=1,z=100,q=2", base64Encode(rgba(1, 1, 0, 255, 0))));
    t.t.feed(apc("a=p,i=10,C=1,q=2")); // only an image on screen animates
    const Image& img = t.g().images().at(10);
    ASSERT_EQ(img.frames.size(), 2u);
    EXPECT_EQ(img.gaps[0], 0u); // the root frame has no gap, as in the protocol
    // new frame: transparent canvas with the patch at x=1
    EXPECT_EQ(img.frames[1]->rgba[3], 0);
    EXPECT_EQ(img.frames[1]->rgba[5], 255);
    EXPECT_EQ(img.gaps[1], 100u);

    t.t.feed(apc("a=a,i=10,r=1,z=40")); // the root frame has no gap until it gets one
    t.t.feed(apc("a=a,i=10,s=3,v=1"));
    EXPECT_TRUE(t.host.written.empty()); // a=a answers only an error, as in kitty
    std::uint64_t now = 1000;
    t.t.advanceAnimations(now); // starts the clock
    EXPECT_EQ(t.g().images().at(10).currentFrame, 0u);
    // frame 0 ends at 1040 (not at the late tick), so frame 1 is due at 1140
    EXPECT_EQ(t.t.advanceAnimations(now + 41), now + 40 + 100);
    EXPECT_EQ(t.g().images().at(10).currentFrame, 1u);
    t.t.advanceAnimations(now + 141);
    EXPECT_EQ(t.g().images().at(10).currentFrame, 0u); // loops
}

TEST(Graphics, FileTransmissionRejectsDevices)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,t=f,f=32,s=1,v=1,i=11", base64Encode("/dev/zero")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=11;EBADF:failed to read image file\x1b\\");
}

TEST(Graphics, ImageStaysAnchoredWhenTheScreenGrows)
{
    // Regression: images were kept at a distance from the bottom, so growing
    // the screen (bottom padding) slid them down.
    Term t(40, 10);
    t.t.feed("\r\n\r\n");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=12,C=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0))));
    auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    ASSERT_EQ(vis[0].y, 40);
    t.t.resize(60, 30, 10, 20); // wider (reflow) and taller
    vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 40);
    t.t.resize(30, 30, 10, 20);
    vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 40);
}

TEST(Graphics, ImageFollowsItsWrappedLine)
{
    Term t(10, 6);
    t.t.feed("0123456789AB"); // wraps onto row 1
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=13,C=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0))));
    auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 20);
    t.t.resize(20, 6, 10, 20); // the line unwraps onto row 0
    vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 0);
}

TEST(Graphics, ImageKeepsItsCharacterColumnWhenTheTerminalReflows)
{
    Term t(10, 6);
    t.t.feed("0123456789AB"); // wraps onto row 1
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=14,C=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0)))); // right after "AB"
    ASSERT_EQ(t.g().placements().size(), 1u);
    EXPECT_EQ(t.g().placements()[0].col, 2);
    t.t.resize(20, 6, 10, 20); // the line unwraps: "AB" ends at column 12
    ASSERT_EQ(t.g().placements().size(), 1u);
    EXPECT_EQ(t.g().placements()[0].col, 12);
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].x, 120);
    EXPECT_EQ(vis[0].y, 0);
}

TEST(Graphics, AlternateScreenImageWhoseRowLeavesOnResizeIsDropped)
{
    Term t(20, 6);
    t.t.feed("\x1b[?1049h\x1b[H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=1,C=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0)))); // row 0
    t.t.feed("\x1b[4;5H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=2,C=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0)))); // row 3, column 4
    t.t.feed("\x1b[6;1H");
    ASSERT_EQ(t.g().placements().size(), 2u);
    t.t.resize(20, 3, 10, 20); // the cursor's row stays: rows 0 to 2 leave at the top
    ASSERT_EQ(t.g().placements().size(), 1u);
    EXPECT_EQ(t.g().placements()[0].imageId, 2u);
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].x, 40);
    EXPECT_EQ(vis[0].y, 0);
}

TEST(Graphics, InsertedLinesMoveImagesAndPushThemOut)
{
    Term t(20, 6);
    t.t.feed(std::string(10, '\n')); // history: IL and DL still move rows inside the screen
    t.t.feed("\x1b[4;1H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=1,C=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0)))); // row 3
    t.t.feed("\x1b[2;1H\x1b[2L"); // IL 2 at row 1: the image's row moves to 5
    auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 100);
    t.t.feed("\x1b[1M"); // DL 1 at row 1: back up to 4
    vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 80);
    t.t.feed("\x1b[2L"); // past the bottom: its row is gone, so is the image
    EXPECT_TRUE(t.g().placements().empty());
    EXPECT_EQ(t.g().images().count(1), 1u); // a numbered image stays for a later placement
}

TEST(Graphics, RegionScrollMovesImagesInsideItOnly)
{
    Term t(20, 6);
    t.t.feed("\x1b[2;5r"); // rows 1 to 4
    t.t.feed("\x1b[1;1H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=1,C=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0)))); // row 0, above the region
    t.t.feed("\x1b[3;1H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=2,C=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0)))); // row 2
    t.t.feed("\x1b[1S"); // SU 1 in the region
    const auto rowOf = [&](std::uint32_t id) {
        for (const auto& v : t.g().visible(t.t.screen(), 10, 20)) {
            if (v.imageId == id) return v.y / 20;
        }
        return -1;
    };
    EXPECT_EQ(rowOf(1), 0);
    EXPECT_EQ(rowOf(2), 1);
    t.t.feed("\x1b[1S"); // row 1 is the region's top: it leaves
    EXPECT_EQ(rowOf(1), 0);
    EXPECT_EQ(rowOf(2), -1);
    EXPECT_EQ(t.g().placements().size(), 1u);
    // Left and right margins: only images in those columns move.
    t.t.feed("\x1b[r\x1b[?69h\x1b[1;10s");
    t.t.feed("\x1b[4;3H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=3,C=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0)))); // row 3, column 2
    t.t.feed("\x1b[4;16H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=4,C=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0)))); // row 3, column 15
    t.t.feed("\x1b[1S");
    EXPECT_EQ(rowOf(3), 2);
    EXPECT_EQ(rowOf(4), 3);
}

TEST(Graphics, UpwardRegionScrollClipsAnImageToTheRowsLeftInside)
{
    Term t(20, 6);
    t.t.feed("\x1b[1;4r\x1b[1;1H"); // rows 0 to 3
    t.t.feed(apc("a=T,f=32,s=10,v=60,i=1,C=1,q=2", base64Encode(rgba(10, 60, 0, 0, 0)))); // rows 0 to 2
    t.t.feed("\x1b[4;1H\n"); // its top-left cell leaves; rows 1 and 2 stay
    auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 0);
    EXPECT_EQ(vis[0].height, 40);
    EXPECT_EQ(vis[0].srcY, 20);
    EXPECT_EQ(vis[0].srcH, 40);
    t.t.feed("\n");
    vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 0);
    EXPECT_EQ(vis[0].height, 20);
    EXPECT_EQ(vis[0].srcY, 40);
    EXPECT_EQ(vis[0].srcH, 20);
    t.t.feed("\n"); // no row is left inside
    EXPECT_TRUE(t.g().placements().empty());
    // An image anchored in the region loses the rows the scroll takes below
    // its bottom margin: they would cover the rows outside the region.
    t.t.feed("\x1b[3;1H");
    t.t.feed(apc("a=T,f=32,s=10,v=80,i=2,C=1,q=2", base64Encode(rgba(10, 80, 0, 0, 0)))); // rows 2 to 5
    t.t.feed("\x1b[4;1H\n");
    vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 20);
    EXPECT_EQ(vis[0].height, 60);
    EXPECT_EQ(vis[0].srcY, 0);
    EXPECT_EQ(vis[0].srcH, 60);
}

TEST(Graphics, ChildOfAClippedImageMovesWithTheRows)
{
    Term t(20, 6);
    t.t.feed(apc("a=t,f=32,s=10,v=60,i=1,q=2", base64Encode(rgba(10, 60, 0, 0, 0))));
    t.t.feed(apc("a=t,f=32,s=10,v=20,i=2,q=2", base64Encode(rgba(10, 20, 0, 0, 0))));
    t.t.feed("\x1b[1;4r\x1b[1;1H"); // rows 0 to 3
    t.t.feed(apc("a=p,i=1,p=1,C=1,q=2")); // rows 0 to 2
    t.t.feed(apc("a=p,i=2,p=2,P=1,Q=1,V=2,q=2")); // row 2
    t.t.feed("\x1b[4;1H\n"); // the parent's anchor moves down to row 0, the child up to row 1
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    const auto child = std::find_if(vis.begin(), vis.end(), [](const auto& v) { return v.imageId == 2; });
    ASSERT_NE(child, vis.end());
    EXPECT_EQ(child->y, 20);
}

TEST(Graphics, ChildOfAClippedImageIsCutLikeItsParent)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=10,v=40,i=1,q=2", base64Encode(rgba(10, 40, 0, 0, 0))));
    t.t.feed(apc("a=t,f=32,s=10,v=20,i=2,q=2", base64Encode(rgba(10, 20, 0, 0, 0))));
    t.t.feed(apc("a=t,f=32,s=10,v=40,i=3,q=2", base64Encode(rgba(10, 40, 0, 0, 0))));
    t.t.feed("\x1b[2;5r\x1b[2;1H"); // rows 1 to 4
    t.t.feed(apc("a=p,i=1,p=1,C=1,q=2")); // rows 1 and 2
    t.t.feed(apc("a=p,i=2,p=1,P=1,Q=1,H=2,q=2")); // row 1, column 2
    t.t.feed(apc("a=p,i=3,p=1,P=1,Q=1,H=4,q=2")); // rows 1 and 2, column 4
    t.t.feed("\x1b[5;1H\n"); // row 1 leaves at the top margin
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    auto find = [&](std::uint32_t id) {
        return std::find_if(vis.begin(), vis.end(), [id](const auto& v) { return v.imageId == id; });
    };
    // The one-row child left with its only row: it does not cover row 0.
    EXPECT_EQ(find(2), vis.end());
    const auto parent = find(1);
    ASSERT_NE(parent, vis.end());
    EXPECT_EQ(parent->y, 20);
    EXPECT_EQ(parent->height, 20);
    EXPECT_EQ(parent->srcY, 20);
    const auto tall = find(3);
    ASSERT_NE(tall, vis.end());
    EXPECT_EQ(tall->x, 40);
    EXPECT_EQ(tall->y, 20);
    EXPECT_EQ(tall->height, 20);
    EXPECT_EQ(tall->srcY, 20);
    EXPECT_EQ(tall->srcH, 20);
    EXPECT_EQ(t.g().placements().size(), 2u);
}

TEST(Graphics, ChildIsCutAtTheBottomMarginAndGoesWithItsParent)
{
    Term t(20, 6);
    t.t.feed(apc("a=t,f=32,s=10,v=20,i=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0))));
    t.t.feed(apc("a=t,f=32,s=10,v=40,i=2,q=2", base64Encode(rgba(10, 40, 0, 0, 0))));
    t.t.feed("\x1b[2;5r\x1b[3;1H"); // rows 1 to 4
    t.t.feed(apc("a=p,i=1,p=1,C=1,q=2")); // row 2
    t.t.feed(apc("a=p,i=2,p=1,P=1,Q=1,V=1,q=2")); // rows 3 and 4
    t.t.feed("\x1b[2;1H\x1bM"); // RI at the top margin: row 4 leaves at the bottom
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    const auto child = std::find_if(vis.begin(), vis.end(), [](const auto& v) { return v.imageId == 2; });
    ASSERT_NE(child, vis.end());
    EXPECT_EQ(child->y, 80);
    EXPECT_EQ(child->height, 20);
    EXPECT_EQ(child->srcY, 0);
    EXPECT_EQ(child->srcH, 20);
    // The parent leaves at the top; the child's row is still inside, but it
    // hangs from the parent.
    t.t.feed("\x1b[3S");
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, DownwardRegionScrollClipsAnImageAtTheBottomMargin)
{
    Term t(20, 6);
    t.t.feed("\x1b[2;5r\x1b[3;1H"); // rows 1 to 4
    // 30 source rows drawn over 3 cell rows: each cell row shows 10 of them.
    t.t.feed(apc("a=T,f=32,s=10,v=30,i=1,r=3,C=1,q=2", base64Encode(rgba(10, 30, 0, 0, 0)))); // rows 2 to 4
    t.t.feed("\x1b[2;1H\x1bM"); // RI at the top margin: row 4 leaves at the bottom
    auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 60);
    EXPECT_EQ(vis[0].height, 40);
    EXPECT_EQ(vis[0].srcY, 0);
    EXPECT_EQ(vis[0].srcH, 20);
    ASSERT_EQ(t.g().placements().size(), 1u);
    EXPECT_EQ(t.g().placements()[0].rows, 2);
    t.t.feed("\x1bM");
    vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 80);
    EXPECT_EQ(vis[0].height, 20);
    EXPECT_EQ(vis[0].srcY, 0);
    EXPECT_EQ(vis[0].srcH, 10);
    t.t.feed("\x1bM"); // no row is left inside
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, UpwardScrollAtTheScreenBottomKeepsRowsBelowIt)
{
    Term t(20, 6);
    t.t.feed(std::string(10, '\n')); // history: SU moves rows inside the screen
    t.t.feed("\x1b[5;1H");
    t.t.feed(apc("a=T,f=32,s=10,v=80,i=1,C=1,q=2", base64Encode(rgba(10, 80, 0, 0, 0)))); // rows 4 to 7, two below the screen
    t.t.feed("\x1b[1S");
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 60);
    EXPECT_EQ(vis[0].height, 80);
    EXPECT_EQ(vis[0].srcH, 80);
}

TEST(Graphics, AlternateScreenRegionScrollMovesImages)
{
    Term t(20, 6);
    t.t.feed("\x1b[?1049h\x1b[2;6r\x1b[4;1H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=1,C=1,q=2", base64Encode(rgba(10, 20, 0, 0, 0)))); // row 3
    t.t.feed("\x1b[6;1H\n"); // a line feed at the bottom margin
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 40);
}

TEST(Graphics, HugeRowCountDoesNotHang)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=14,q=2", base64Encode(rgba(1, 1, 0, 0, 0))));
    t.t.feed(apc("a=p,i=14,r=2000000000,q=2"));
    EXPECT_EQ(t.t.screen().cursor.y, 9);
}

TEST(Graphics, UnicodePlaceholders)
{
    Term t(20, 10);
    // A 40x40 image as a virtual placement of 4x2 cells (10x20 px cells).
    t.t.feed(apc("a=t,f=32,s=40,v=40,i=3,q=2", base64Encode(rgba(40, 40, 1, 2, 3))));
    t.t.feed(apc("a=p,i=3,U=1,c=4,r=2,q=2"));
    EXPECT_EQ(AfterglowProto::placeholderDiacriticValue(0x0305), 0);
    EXPECT_EQ(AfterglowProto::placeholderDiacriticValue(0x030D), 1);
    EXPECT_EQ(AfterglowProto::placeholderDiacriticValue(U'a'), -1);
    // Cell (row 1, col 2): 40x40 fits a 40x40 grid exactly, so the slice is
    // image pixels (20, 20)-(30, 40).
    auto slice = t.g().placeholderSlice(3, 0, 1, 2, 0, 0, 10, 20);
    ASSERT_TRUE(slice.has_value());
    EXPECT_EQ(slice->srcX, 20);
    EXPECT_EQ(slice->srcY, 20);
    EXPECT_EQ(slice->srcW, 10);
    EXPECT_EQ(slice->srcH, 20);
    EXPECT_FALSE(t.g().placeholderSlice(3, 0, 2, 0, 0, 0, 10, 20).has_value()); // outside the grid
    EXPECT_FALSE(t.g().placeholderSlice(99, 0, 0, 0, 0, 0, 10, 20).has_value());
}

TEST(Graphics, RelativePlacementsFollowParentAndCascadeDeletion)
{
    Term t(40, 20);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1", base64Encode(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=2", base64Encode(rgba(1, 1, 9, 8, 7))));
    t.t.feed("\x1b[4;5H");
    t.t.feed(apc("a=p,i=1,p=1,C=1"));
    t.t.feed(apc("a=p,i=2,p=2,P=1,Q=1,H=-2,V=3"));
    EXPECT_EQ(t.t.screen().cursor.x, 4);
    EXPECT_EQ(t.t.screen().cursor.y, 3);
    auto visible = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(visible.size(), 2u);
    EXPECT_EQ(visible[1].x, 20);
    EXPECT_EQ(visible[1].y, 120);
    t.t.feed("\x1b[6;8H");
    t.t.feed(apc("a=p,i=1,p=1,C=1"));
    visible = t.g().visible(t.t.screen(), 10, 20);
    auto child = std::find_if(visible.begin(), visible.end(), [](const auto& v) { return v.imageId == 2; });
    ASSERT_NE(child, visible.end());
    EXPECT_EQ(child->x, 50);
    EXPECT_EQ(child->y, 160);
    t.t.feed(apc("a=d,d=i,i=1,p=1"));
    // The child goes with its parent; a lowercase delete keeps image data,
    // the child's included.
    EXPECT_TRUE(t.g().placements().empty());
    EXPECT_EQ(t.g().images().count(2), 1u);
    t.host.written.clear();
    t.t.feed(apc("a=p,i=2,C=1"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;OK\x1b\\");
}

TEST(Graphics, RelativeErrorsDoNotReplaceExistingPlacement)
{
    Term t(40, 20);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1", base64Encode(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=p,i=1,p=1,C=1"));
    t.t.feed(apc("a=p,i=1,p=2,P=1,Q=1"));
    t.host.written.clear();
    t.t.feed(apc("a=p,i=1,p=1,P=1,Q=2"));
    EXPECT_NE(t.host.written.find("ECYCLE"), std::string::npos);
    EXPECT_EQ(t.g().placements().size(), 2u);
    t.host.written.clear();
    t.t.feed(apc("a=p,i=1,p=3,P=99"));
    EXPECT_NE(t.host.written.find("ENOPARENT"), std::string::npos);
    t.host.written.clear();
    t.t.feed(apc("a=p,i=1,p=3,P=1,Q=1,U=1"));
    EXPECT_NE(t.host.written.find("EINVAL"), std::string::npos);
    for (int i = 3; i <= 33; ++i) {
        t.t.feed(apc("a=p,i=1,p=" + std::to_string(i) + ",P=1,Q=" + std::to_string(i - 1)));
    }
    EXPECT_EQ(t.g().placements().size(), 33u);
    t.host.written.clear();
    t.t.feed(apc("a=p,i=1,p=34,P=1,Q=33"));
    EXPECT_NE(t.host.written.find("ETOODEEP"), std::string::npos);
}

TEST(Graphics, RelativeVirtualParentTracksPlaceholderText)
{
    Term t(40, 20);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1", base64Encode(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=p,i=1,p=1,U=1,c=1,r=1"));
    t.t.feed(apc("a=p,i=1,p=2,P=1,Q=1,H=2,V=1"));
    EXPECT_TRUE(t.g().visible(t.t.screen(), 10, 20).empty());
    t.t.feed("\x1b[3;4H\x1b[38;2;0;0;1m\xf4\x8e\xbb\xae\x1b[0m");
    auto visible = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].x, 50);
    EXPECT_EQ(visible[0].y, 60);
    t.t.feed("\x1b[3;1H\x1b[2@");
    visible = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].x, 70);
}

TEST(Graphics, TransmitAndPlaceReportsInvalidRelativeParent)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=1,P=99", base64Encode(rgba(1, 1, 1, 2, 3))));
    EXPECT_NE(t.host.written.find("ENOPARENT"), std::string::npos);
    EXPECT_EQ(t.host.written.find("OK"), std::string::npos);
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, SharedMemoryOffsetWithoutSizeReadsRemainingBytes)
{
    const ShmName shm{"/afterglow-graphics-test-" + std::to_string(getpid())};
    const int fd = shm_open(shm.name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(write(fd, "skipRGBA", 8), 8);
    close(fd);
    Term t(20, 10);
    t.t.feed(apc("a=t,t=s,f=32,s=1,v=1,O=4,i=7", base64Encode(shm.name)));
    ASSERT_EQ(t.g().images().count(7), 1u);
    EXPECT_EQ(t.g().images().at(7).frames[0]->rgba[0], 'R');
}

TEST(Graphics, SharedMemoryIsDeletedOnlyWhenNamedForTheProtocol)
{
    // Almost any object decodes as a tiny image, so a decode proves nothing:
    // an object without the protocol's tag in its name is never deleted.
    const ShmName plain{"/afterglow-graphics-plain-" + std::to_string(getpid())};
    const ShmName tagged{"/afterglow-tty-graphics-protocol-test-" + std::to_string(getpid())};
    for (const ShmName* shm : {&plain, &tagged}) {
        const int fd = shm_open(shm->name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(write(fd, "victim", 6), 6);
        close(fd);
    }
    Term t(20, 10);
    t.t.feed(apc("a=t,i=1,t=s,f=24,s=1,v=1", base64Encode(plain.name)));
    t.t.feed(apc("a=t,i=2,t=s,f=24,s=1,v=1", base64Encode(tagged.name)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;OK\x1b\\\x1b_Gi=2;OK\x1b\\");
    const int survivor = shm_open(plain.name.c_str(), O_RDONLY, 0);
    EXPECT_GE(survivor, 0);
    if (survivor >= 0) close(survivor);
    EXPECT_EQ(shm_open(tagged.name.c_str(), O_RDONLY, 0), -1);
}

TEST(Graphics, FilePayloadRejectsSparseOversizeAndNonRegularFiles)
{
    std::string pattern = testing::TempDir() + "afterglow-graphics-test-XXXXXX";
    char* name = pattern.data();
    const int fd = mkstemp(name);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, GraphicsStore::DefaultQuota + 1), 0);
    close(fd);
    Term t(20, 10);
    // PNG data is used whole (raw pixels read only the image's bytes).
    // Too long, a FIFO, or missing: one answer, which tells the client
    // nothing about a file it may not be able to read itself.
    t.t.feed(apc("a=t,t=f,f=100,i=7", base64Encode(name)));
    EXPECT_TRUE(t.g().images().empty());
    EXPECT_EQ(t.host.written, "\x1b_Gi=7;EBADF:failed to read image file\x1b\\");
    unlink(name);
    ASSERT_EQ(mkfifo(name, 0600), 0);
    t.host.written.clear();
    t.t.feed(apc("a=t,t=f,f=32,s=1,v=1,i=8", base64Encode(name)));
    EXPECT_TRUE(t.g().images().empty());
    EXPECT_EQ(t.host.written, "\x1b_Gi=8;EBADF:failed to read image file\x1b\\");
    unlink(name);
    t.host.written.clear();
    t.t.feed(apc("a=t,t=f,f=32,s=1,v=1,i=9", base64Encode(name)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=9;EBADF:failed to read image file\x1b\\");
}

TEST(Graphics, ExtendedViewportIncludesImageBelowVisibleRows)
{
    Term t(20, 3);
    t.t.feed("\n\n\n");
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=8,C=1,q=2", base64Encode(rgba(1, 1, 0, 0, 0))));
    t.t.screen().scrollViewport(1);
    EXPECT_TRUE(t.g().visible(t.t.screen(), 10, 20).empty());
    EXPECT_EQ(t.g().visible(t.t.screen(), 10, 20, 1).size(), 1u);
}

TEST(Graphics, PlacementMetadataAndHostileGeometryAreBounded)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 0, 0, 0))));
    for (int i = 1; i <= 4100; ++i) {
        t.t.feed(apc("a=p,i=1,C=1,q=2,p=" + std::to_string(i)));
    }
    EXPECT_EQ(t.g().placements().size(), 4096u);
    t.t.feed(apc("a=p,i=1,p=1,c=2147483647,r=2147483647,X=2147483647,Y=2147483647,C=1,q=2"));
    ASSERT_EQ(t.g().placements().size(), 4096u); // replaced, not added
    // The huge offsets stay inside the first cell, and the huge size is
    // bounded: every placement is drawn, with sane sizes.
    EXPECT_EQ(t.g().placements()[0].offsetX, 9);
    EXPECT_EQ(t.g().placements()[0].offsetY, 19);
    const auto vis = t.g().visible(t.t.screen(), 65535, 65535);
    EXPECT_EQ(vis.size(), 4096u);
    for (const auto& v : vis) {
        ASSERT_GT(v.width, 0);
        ASSERT_GT(v.height, 0);
        ASSERT_EQ(v.srcW, 1);
    }
    t.t.feed(apc("a=d,d=x,x=1")); // every placement starts in column 1
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, SupportQueryAnswersWithoutStoring)
{
    Term t(20, 10);
    t.t.feed(apc("a=q,i=31,s=1,v=1,f=24", base64Encode("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=31;OK\x1b\\");
    EXPECT_TRUE(t.g().images().empty());
    t.host.written.clear();
    const std::string data = base64Encode("abcdef"); // two pixels
    t.t.feed(apc("a=q,i=32,s=2,v=1,f=24,m=1", data.substr(0, 4)));
    t.t.feed(apc("m=0", data.substr(4)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=32;OK\x1b\\");
    EXPECT_TRUE(t.g().images().empty());
}

TEST(Graphics, ComposeCopiesARectangleBetweenFrames)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=2,v=1,i=1,q=2", base64Encode(rgba(2, 1, 255, 0, 0))));
    t.t.feed(apc("a=f,f=32,s=2,v=1,i=1,q=2", base64Encode(rgba(2, 1, 0, 0, 255))));
    ASSERT_EQ(t.g().images().at(1).frames.size(), 2u);
    t.host.written.clear();
    // Frame 1's pixel (0, 0) onto frame 2 at (1, 0): x, y place the
    // destination, X, Y the source.
    t.t.feed(apc("a=c,i=1,r=1,c=2,w=1,h=1,x=1"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;OK\x1b\\");
    const auto& second = t.g().images().at(1).frames[1]->rgba;
    EXPECT_EQ(second[0], 0); // (0, 0) stays blue
    EXPECT_EQ(second[2], 255);
    EXPECT_EQ(second[4], 255); // (1, 0) is red now
    EXPECT_EQ(second[6], 0);
    t.host.written.clear();
    t.t.feed(apc("a=c,i=1,r=3,c=1"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;ENOENT:no such frame\x1b\\");
}

TEST(Graphics, ComposeReadsTheSourceAtCapitalXAndY)
{
    Term t(20, 10);
    std::string left = rgba(1, 1, 255, 0, 0) + rgba(1, 1, 0, 255, 0); // red, green
    t.t.feed(apc("a=t,f=32,s=2,v=1,i=1,q=2", base64Encode(left)));
    t.t.feed(apc("a=f,f=32,s=2,v=1,i=1,q=2", base64Encode(rgba(2, 1, 0, 0, 255))));
    // Frame 1's pixel (1, 0), green, onto frame 2 at (0, 0).
    t.t.feed(apc("a=c,i=1,r=1,c=2,w=1,h=1,X=1,C=1,q=2"));
    const auto& second = t.g().images().at(1).frames[1]->rgba;
    EXPECT_EQ(second[0], 0);
    EXPECT_EQ(second[1], 255);
    EXPECT_EQ(second[4], 0); // (1, 0) stays blue
    EXPECT_EQ(second[6], 255);
}

TEST(Graphics, ComposeRefusesRectanglesOutsideTheImageOrOverlapping)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=4,v=4,i=1,q=2", base64Encode(rgba(4, 4, 255, 0, 0))));
    t.t.feed(apc("a=f,f=32,s=4,v=4,i=1,q=2", base64Encode(rgba(4, 4, 0, 0, 255))));
    const auto before = t.g().images().at(1).frames[1];
    for (std::string_view keys : {"a=c,i=1,r=1,c=2,w=2,h=2,x=3",  // destination past the right edge
                                  "a=c,i=1,r=1,c=2,w=2,h=2,Y=3",  // source past the bottom
                                  "a=c,i=1,r=1,c=2,w=5",          // wider than the image
                                  "a=c,i=1,r=2,c=2,w=2,h=2,x=1,y=1"}) { // one frame, overlapping
        t.host.written.clear();
        t.t.feed(apc(keys));
        EXPECT_NE(t.host.written.find(";EINVAL:"), std::string::npos) << keys << ": " << t.host.written;
    }
    EXPECT_EQ(t.g().images().at(1).frames[1], before); // nothing was composed
    // In one frame, rectangles side by side do not overlap.
    t.host.written.clear();
    t.t.feed(apc("a=c,i=1,r=2,c=2,w=2,h=2,x=2,y=2"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;OK\x1b\\");
}

namespace {
struct PngHost : RecordingHost
{
    std::uint32_t width = 2;
    std::uint32_t height = 2;
    std::size_t bytes = 16;
    bool decodePng(std::string_view, std::uint32_t& w, std::uint32_t& h, std::vector<std::uint8_t>& rgba) override
    {
        w = width;
        h = height;
        rgba.assign(bytes, 7);
        return true;
    }
};
} // namespace

TEST(Graphics, HostDecodedPngIsChecked)
{
    PngHost host;
    Terminal t(20, 10, host);
    t.resize(20, 10, 10, 20);
    t.feed(apc("a=t,f=100,i=1", base64Encode("png")));
    EXPECT_EQ(host.written, "\x1b_Gi=1;OK\x1b\\");
    host.written.clear();
    host.width = 10001;
    host.height = 1;
    host.bytes = 10001 * 4;
    t.feed(apc("a=t,f=100,i=2", base64Encode("png")));
    EXPECT_EQ(host.written, "\x1b_Gi=2;EINVAL:invalid image dimensions\x1b\\");
    host.written.clear();
    host.width = 2;
    host.height = 2;
    host.bytes = 15;
    t.feed(apc("a=t,f=100,i=3", base64Encode("png")));
    EXPECT_EQ(host.written, "\x1b_Gi=3;EINVAL:invalid decoded image extent\x1b\\");
    RecordingHost plain; // decodes no PNG at all
    Terminal u(20, 10, plain);
    u.feed(apc("a=t,f=100,i=4", base64Encode("png")));
    EXPECT_EQ(plain.written, "\x1b_Gi=4;EBADPNG:failed to decode PNG data\x1b\\");
}

TEST(Graphics, CompressedPngInflatesOnlyToTheQuota)
{
    // A PNG declares no size before it is decoded: o=z data may inflate as
    // far as the store could keep, here 1,000 bytes.
    PngHost host;
    TerminalOptions o;
    o.graphicsQuota = 1000;
    Terminal t(20, 10, host, o);
    t.resize(20, 10, 10, 20);
    t.feed(apc("a=t,f=100,o=z,i=1", base64Encode(zlibCompress(std::string(1000, 'p')))));
    EXPECT_EQ(host.written, "\x1b_Gi=1;OK\x1b\\");
    host.written.clear();
    t.feed(apc("a=t,f=100,o=z,i=2", base64Encode(zlibCompress(std::string(1001, 'p')))));
    EXPECT_EQ(host.written, "\x1b_Gi=2;EINVAL:zlib decompression failed\x1b\\");
}

TEST(Graphics, DeleteModesSelectByPositionAndZ)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 1, 1, 1))));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=2,q=2", base64Encode(rgba(1, 1, 2, 2, 2))));
    // Image 1 at row 3, column 4 with z 5; image 2 at row 7, column 9 with z -3.
    auto placeBoth = [&] {
        t.t.feed("\x1b[3;4H" + apc("a=p,i=1,p=1,z=5,C=1,q=2"));
        t.t.feed("\x1b[7;9H" + apc("a=p,i=2,p=1,z=-3,C=1,q=2"));
        ASSERT_EQ(t.g().placements().size(), 2u);
    };
    auto left = [&] {
        EXPECT_EQ(t.g().placements().size(), 1u);
        return t.g().placements().empty() ? 0u : t.g().placements()[0].imageId;
    };
    placeBoth();
    t.t.feed(apc("a=d,d=y,y=3"));
    EXPECT_EQ(left(), 2u);
    placeBoth();
    t.t.feed(apc("a=d,d=x,x=9"));
    EXPECT_EQ(left(), 1u);
    placeBoth();
    t.t.feed(apc("a=d,d=p,x=4,y=3"));
    EXPECT_EQ(left(), 2u);
    placeBoth();
    t.t.feed(apc("a=d,d=q,x=4,y=3,z=4"));
    EXPECT_EQ(t.g().placements().size(), 2u); // z does not match
    t.t.feed(apc("a=d,d=q,x=4,y=3,z=5"));
    EXPECT_EQ(left(), 2u);
    placeBoth();
    t.t.feed(apc("a=d,d=z,z=-3"));
    EXPECT_EQ(left(), 1u);
    placeBoth();
    t.t.feed("\x1b[3;4H" + apc("a=d,d=c"));
    EXPECT_EQ(left(), 2u);
    placeBoth();
    t.t.feed(apc("a=d,d=a"));
    EXPECT_TRUE(t.g().placements().empty());
    // Lowercase kinds keep the data; uppercase ones free what they unplace.
    EXPECT_EQ(t.g().images().size(), 2u);
    placeBoth();
    t.t.feed(apc("a=d,d=Z,z=-3"));
    EXPECT_EQ(left(), 1u);
    EXPECT_EQ(t.g().images().count(2), 0u);
    EXPECT_EQ(t.g().images().count(1), 1u);
    t.t.feed(apc("a=t,f=32,s=1,v=1,I=7,q=2", base64Encode(rgba(1, 1, 3, 3, 3))));
    t.t.feed(apc("a=p,I=7,C=1,q=2"));
    ASSERT_EQ(t.g().placements().size(), 2u);
    t.t.feed(apc("a=d,d=N,I=7"));
    EXPECT_EQ(left(), 1u);
    EXPECT_EQ(t.g().images().size(), 1u);
}

TEST(Graphics, UnsupportedDeleteKindsAreRefused)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 1, 1, 1))));
    t.t.feed(apc("a=d,d=w,i=1"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;EINVAL:unsupported delete target\x1b\\");
}

TEST(Graphics, ImageWhoseRowLeavesTheBufferOnReflowIsDropped)
{
    TerminalOptions o;
    o.maxScrollback = 0;
    Term t(10, 4, o);
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=1,C=1,q=2", base64Encode(rgba(1, 1, 0, 0, 0))));
    t.t.feed("\r\n0123456789\r\n0123456789\r\n0123456789");
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.resize(3, 4, 10, 20); // the lines below need 12 rows: row 0 leaves
    EXPECT_TRUE(t.g().placements().empty());
    EXPECT_EQ(t.g().images().count(1), 1u);
}

TEST(Graphics, OmittedParentPlacementBindsAtCreation)
{
    Term t(40, 20);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=2,q=2", base64Encode(rgba(1, 1, 9, 8, 7))));
    t.t.feed("\x1b[2;2H" + apc("a=p,i=1,p=1,C=1,q=2"));
    t.t.feed("\x1b[6;6H" + apc("a=p,i=1,p=2,C=1,q=2"));
    t.t.feed(apc("a=p,i=2,p=1,P=1,H=1,q=2")); // no Q: the first placement of image 1
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    const auto child = std::find_if(vis.begin(), vis.end(), [](const auto& v) { return v.imageId == 2; });
    ASSERT_NE(child, vis.end());
    EXPECT_EQ(child->x, 20);
    EXPECT_EQ(child->y, 20);
    t.t.feed(apc("a=d,d=i,i=1,p=1"));
    // The child was bound to p=1 and goes with it, not over to p=2.
    ASSERT_EQ(t.g().placements().size(), 1u);
    EXPECT_EQ(t.g().placements()[0].imageId, 1u);
    EXPECT_EQ(t.g().placements()[0].placementId, 2u);
}

TEST(Graphics, UnfinishedChunkedUploadDoesNotSwallowTheNextCommand)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,i=1,f=24,s=1,v=1,m=1", "AAAA")); // the sender dies here
    t.t.feed(apc("a=q,i=31,s=1,v=1,f=24", base64Encode("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=31;OK\x1b\\");
    EXPECT_TRUE(t.g().images().empty());
    // A reset ends a transfer too.
    t.host.written.clear();
    t.t.feed(apc("a=t,i=2,f=24,s=1,v=1,m=1", "AA"));
    t.t.feed("\x1b" "c");
    t.t.feed(apc("a=t,i=3,f=24,s=1,v=1", base64Encode("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=3;OK\x1b\\");
    EXPECT_EQ(t.g().images().count(2), 0u);
    // A transfer left unfinished on the other screen.
    t.host.written.clear();
    t.t.feed(apc("a=t,i=4,f=24,s=1,v=1,m=1", "AA"));
    t.t.feed("\x1b[?1049h");
    t.t.feed(apc("a=t,i=5,f=24,s=1,v=1", base64Encode("abc")));
    t.t.feed("\x1b[?1049l");
    t.t.feed(apc("a=t,i=6,f=24,s=1,v=1", base64Encode("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=5;OK\x1b\\\x1b_Gi=6;OK\x1b\\");
    EXPECT_EQ(t.g().images().count(4), 0u);
    EXPECT_EQ(t.g().images().count(6), 1u);
}

TEST(Graphics, AnonymousAndInternalImagesGoWithTheirLastPlacement)
{
    Term t(20, 10);
    for (int i = 0; i < 3; ++i) {
        t.t.feed(apc("a=T,f=24,s=1,v=1", base64Encode("abc")));
    }
    t.t.feed("\x1b[5;1H\x1bPq#1~\x1b\\");
    ASSERT_EQ(t.g().placements().size(), 4u);
    ASSERT_EQ(t.g().images().size(), 4u);
    t.t.feed("\x1b[2J");
    EXPECT_TRUE(t.g().placements().empty());
    EXPECT_TRUE(t.g().images().empty());
}

TEST(Graphics, TheImageLimitMakesRoomFromImagesNotShown)
{
    Term t(20, 10);
    for (std::size_t i = 1; i <= GraphicsStore::MaxImages; ++i) {
        t.t.feed(apc("a=t,f=24,s=1,v=1,q=2,i=" + std::to_string(i), base64Encode("abc")));
    }
    t.t.feed(apc("a=p,i=1,C=1,q=2"));
    t.host.written.clear();
    // A command that fails costs no image.
    t.t.feed(apc("a=t,f=24,s=2,v=2,i=4999", base64Encode("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=4999;ENODATA:insufficient image data\x1b\\");
    EXPECT_EQ(t.g().images().count(2), 1u);
    t.host.written.clear();
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=5000", base64Encode("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=5000;OK\x1b\\");
    EXPECT_EQ(t.g().images().size(), GraphicsStore::MaxImages);
    EXPECT_EQ(t.g().images().count(1), 1u); // shown, so kept
    EXPECT_EQ(t.g().images().count(2), 0u); // the oldest not shown made room
}

TEST(Graphics, PlacementsThatLeftTheScrollbackLeaveTheLimit)
{
    TerminalOptions o;
    o.maxScrollback = 2;
    Term t(20, 2, o);
    t.t.feed(apc("a=t,i=1,f=24,s=1,v=1,q=2", base64Encode("abc")));
    for (std::size_t i = 0; i < GraphicsStore::MaxPlacements; ++i) {
        t.t.feed(apc("a=p,i=1,C=1,q=2") + "\n\n\n\n");
    }
    t.host.written.clear();
    t.t.feed(apc("a=p,i=1,C=1"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;OK\x1b\\");
    EXPECT_LE(t.g().placements().size(), 2u);
}

TEST(Graphics, ImageStaysWhileItsLowerRowsAreOnScreen)
{
    Term t(20, 10);
    t.t.feed("\x1b[?1049h\x1b[2;3H");
    // 3 rows high, anchored on row 1 of a screen with no history.
    t.t.feed(apc("a=T,f=32,s=25,v=45,i=5,q=2", base64Encode(rgba(25, 45, 0, 0, 255))));
    t.t.feed("\x1b[10;1H\n\n");
    auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, -20);
    t.t.feed("\n\n"); // now every row has left
    EXPECT_TRUE(t.g().visible(t.t.screen(), 10, 20).empty());
}

TEST(Graphics, ResizeKeepsAnImageWhoseTopRowLeft)
{
    Term t(20, 10);
    t.t.feed("\x1b[?1049h\x1b[2;3H");
    t.t.feed(apc("a=T,f=32,s=25,v=45,i=5,q=2", base64Encode(rgba(25, 45, 0, 0, 255))));
    t.t.feed("\x1b[10;1H\n\n"); // its top row has left; two rows still show
    t.t.resize(20, 12, 10, 20);
    auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, -20);
    t.t.resize(30, 12, 10, 20); // the alternate screen does not reflow either
    vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, -20);
}

TEST(Graphics, QuotaEvictsOlderImagesNotTheOneBeingAdded)
{
    TerminalOptions o;
    o.graphicsQuota = 3000;
    Term t(20, 10, o);
    t.t.feed(apc("a=T,f=32,s=30,v=20,i=1,C=1,q=2", base64Encode(rgba(30, 20, 1, 2, 3)))); // 2,400 bytes, shown
    t.host.written.clear();
    t.t.feed(apc("a=T,f=32,s=25,v=20,i=2,C=1", base64Encode(rgba(25, 20, 4, 5, 6)))); // 2,000 bytes
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;OK\x1b\\");
    EXPECT_EQ(t.g().images().count(1), 0u);
    EXPECT_EQ(t.g().images().count(2), 1u);
    t.host.written.clear();
    t.t.feed(apc("a=t,f=32,s=30,v=30,i=3", base64Encode(rgba(30, 30, 0, 0, 0)))); // 3,600 bytes: never fits
    EXPECT_EQ(t.host.written, "\x1b_Gi=3;ENOSPC:image larger than the storage quota\x1b\\");
    EXPECT_EQ(t.g().images().count(2), 1u);
}

TEST(Graphics, TransientImagesGoFirstWhenTheQuotaIsFull)
{
    TerminalOptions o;
    o.graphicsQuota = 5000;
    Term t(20, 10, o);
    t.t.feed(apc("a=t,f=32,s=20,v=20,i=1,q=2", base64Encode(rgba(20, 20, 1, 1, 1)))); // 1,600 bytes each
    t.t.feed(apc("a=t,f=32,s=20,v=20,i=2,N=1,q=2", base64Encode(rgba(20, 20, 2, 2, 2))));
    t.t.feed(apc("a=T,f=32,s=20,v=20,i=3,N=1,C=1,q=2", base64Encode(rgba(20, 20, 3, 3, 3)))); // shown
    ASSERT_EQ(t.g().images().size(), 3u);
    EXPECT_FALSE(t.g().images().at(1).transient);
    EXPECT_TRUE(t.g().images().at(2).transient);
    // Room for a fourth: the transient image that nothing shows goes, not
    // the older image without the hint, and not the shown transient one.
    t.t.feed(apc("a=t,f=32,s=20,v=20,i=4,q=2", base64Encode(rgba(20, 20, 4, 4, 4))));
    EXPECT_EQ(t.g().images().count(1), 1u);
    EXPECT_EQ(t.g().images().count(2), 0u);
    EXPECT_EQ(t.g().images().count(3), 1u);
    EXPECT_EQ(t.g().images().count(4), 1u);
    // A frame does not set the hint, and sending the image again without it
    // clears it.
    t.t.feed(apc("a=f,i=1,N=1,q=2", base64Encode(rgba(20, 20, 5, 5, 5))));
    EXPECT_FALSE(t.g().images().at(1).transient);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=3,q=2", base64Encode(rgba(1, 1, 6, 6, 6))));
    EXPECT_FALSE(t.g().images().at(3).transient);
}

TEST(Graphics, TransientImagesGoFirstAtTheImageLimit)
{
    Term t(20, 10);
    for (std::uint32_t id = 1; id <= GraphicsStore::MaxImages; ++id) {
        const std::string hint = id == 7 ? ",N=1" : "";
        t.t.feed(apc("a=t,f=32,s=1,v=1,q=2,i=" + std::to_string(id) + hint, base64Encode(rgba(1, 1, 0, 0, 0))));
    }
    ASSERT_EQ(t.g().images().size(), std::size_t(GraphicsStore::MaxImages));
    t.t.feed(apc("a=t,f=32,s=1,v=1,q=2,i=99999", base64Encode(rgba(1, 1, 0, 0, 0))));
    EXPECT_EQ(t.g().images().count(99999), 1u);
    EXPECT_EQ(t.g().images().count(7), 0u);
    EXPECT_EQ(t.g().images().count(1), 1u); // the oldest stays: it has no hint
}

TEST(Graphics, IndexedFormatsKeepTheirIndices)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=8,s=2,v=1,i=1", base64Encode(std::string("\x01\xf8", 2))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;OK\x1b\\");
    const ImageFrame& opaque = *t.g().images().at(1).frames.front();
    EXPECT_EQ(opaque.format, PixelFormat::Indexed);
    EXPECT_EQ(opaque.indices, (std::vector<std::uint8_t>{1, 248}));
    EXPECT_TRUE(opaque.rgba.empty());
    EXPECT_EQ(opaque.byteSize(), 8u); // the quota counts the RGBA that is drawn
    // f=16: an index and an alpha for each pixel, here compressed.
    t.host.written.clear();
    t.t.feed(apc("a=t,f=16,s=1,v=2,o=z,i=2", base64Encode(zlibCompress(std::string("\x05\x80\xfc\xff", 4)))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;OK\x1b\\");
    const ImageFrame& alpha = *t.g().images().at(2).frames.front();
    EXPECT_EQ(alpha.format, PixelFormat::IndexedAlpha);
    EXPECT_EQ(alpha.indices, (std::vector<std::uint8_t>{5, 0x80, 0xfc, 0xff}));
    // A query checks support and stores nothing.
    t.host.written.clear();
    t.t.feed(apc("a=q,f=8,s=1,v=1,i=3", base64Encode(std::string("\x07", 1))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=3;OK\x1b\\");
    EXPECT_EQ(t.g().images().count(3), 0u);
    // In chunks, with data past the image: the image takes its 7 indices.
    t.host.written.clear();
    const std::string data = base64Encode(std::string("\x01\x02\x03\x04\x05\x06\x07\x08\x09", 9));
    t.t.feed(apc("a=t,f=8,s=7,v=1,i=4,m=1", data.substr(0, 4)));
    t.t.feed(apc("m=1", data.substr(4, 4)));
    t.t.feed(apc("m=1", data.substr(8)));
    t.t.feed(apc("m=0", base64Encode("tail")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=4;OK\x1b\\");
    EXPECT_EQ(t.g().images().at(4).frames.front()->indices, (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7}));
}

TEST(Graphics, IndexedFormatsAreCheckedLikeRawPixels)
{
    TerminalOptions o;
    o.graphicsQuota = 36;
    Term t(20, 10, o);
    t.t.feed(apc("a=t,f=8,i=1", base64Encode("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;EINVAL:missing or invalid image size\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=t,f=16,s=2,v=1,i=2", base64Encode("abc"))); // 3 bytes of 4
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;ENODATA:insufficient image data\x1b\\");
    // The quota counts 4 bytes for each pixel, the RGBA that is drawn:
    // 3x3 fits in 36 bytes for both formats.
    t.host.written.clear();
    t.t.feed(apc("a=t,f=8,s=3,v=3,i=3", base64Encode(std::string(9, '\x01'))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=3;OK\x1b\\");
    EXPECT_EQ(t.g().totalBytes(), 36u);
    t.host.written.clear();
    t.t.feed(apc("a=t,f=16,s=3,v=3,i=4", base64Encode(std::string(18, '\x01'))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=4;OK\x1b\\");
    EXPECT_EQ(t.g().totalBytes(), 36u); // image 3 had no placement: it went
    // 10 pixels do not fit. The size is checked before anything inflates:
    // data that is not zlib gets the quota's error, not the stream's.
    for (const char* keys : {"a=t,f=8,s=2,v=5,o=z,i=5", "a=t,f=16,s=2,v=5,o=z,i=5"}) {
        t.host.written.clear();
        t.t.feed(apc(keys, base64Encode("not zlib")));
        EXPECT_EQ(t.host.written, "\x1b_Gi=5;ENOSPC:image larger than the storage quota\x1b\\") << keys;
    }
    // A stream inflates to the size of the indices and no further.
    t.host.written.clear();
    t.t.feed(apc("a=t,f=8,s=1,v=1,o=z,i=6", base64Encode(zlibCompress("ab"))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=6;EINVAL:zlib decompression failed\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=t,f=16,s=1,v=1,o=z,i=6", base64Encode(zlibCompress("ab"))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=6;OK\x1b\\");
    // An unknown format still fails.
    t.host.written.clear();
    t.t.feed(apc("a=t,f=12,s=1,v=1,i=7", base64Encode("ab")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=7;EINVAL:unknown format\x1b\\");
    // f=0 is the default format, as in kitty.
    t.host.written.clear();
    t.t.feed(apc("a=t,f=0,s=1,v=1,i=8", base64Encode(rgba(1, 1, 1, 2, 3))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=8;OK\x1b\\");
    EXPECT_EQ(t.g().images().at(8).frames.front()->format, PixelFormat::Rgba);
}

TEST(Graphics, IndexedFileIsReadOnlyAsFarAsTheImage)
{
    // 2x1 at f=16 is 4 bytes. A work budget of 4 bytes with no refill
    // refuses a read of any more, so the OK shows that no more was read.
    TempFile file("afterglow-graphics-indexed-");
    ASSERT_TRUE(file.write(std::string("\x01\x80\x02\xff" "more", 8)));
    Store s(20, 5);
    s.g.setWorkBudget(4, 0);
    s.apc("a=t,t=f,f=16,s=2,v=1,i=1", base64Encode(file.path));
    EXPECT_EQ(s.host.written, "\x1b_Gi=1;OK\x1b\\");
    ASSERT_EQ(s.g.images().count(1), 1u);
    EXPECT_EQ(s.g.images().at(1).frames.front()->indices, (std::vector<std::uint8_t>{1, 0x80, 2, 0xff}));
}

TEST(Graphics, IndexedImagesHaveNoFrames)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=8,s=1,v=1,i=1,q=2", base64Encode(std::string("\x01", 1))));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=2,q=2", base64Encode(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=f,i=1,f=32,s=1,v=1", base64Encode(rgba(1, 1, 1, 2, 3))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1,r=2;EINVAL:indexed images have no frames\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=f,i=2,f=8,s=1,v=1", base64Encode(std::string("\x01", 1))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=2,r=2;EINVAL:indexed images have no frames\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=c,i=1,r=1,c=1"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;EINVAL:indexed images have no frames\x1b\\");
    EXPECT_EQ(t.g().images().at(1).frames.size(), 1u);
    EXPECT_EQ(t.g().images().at(2).frames.size(), 1u);
    // A frame by image number names the image, and its temporary file goes.
    t.t.feed(apc("a=t,f=8,s=1,v=1,I=13,q=2", base64Encode(std::string("\x01", 1))));
    ASSERT_EQ(t.g().images().size(), 3u);
    std::uint32_t id = 0;
    for (const auto& [key, image] : t.g().images()) {
        if (key != 1 && key != 2) id = key;
    }
    TempFile file("afterglow-tty-graphics-protocol-indexed-");
    ASSERT_TRUE(file.write("RGBA"));
    t.host.written.clear();
    t.t.feed(apc("a=f,I=13,t=t,f=32,s=1,v=1", base64Encode(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=" + std::to_string(id) + ",I=13,r=2;EINVAL:indexed images have no frames\x1b\\");
    EXPECT_FALSE(std::filesystem::exists(file.path));
}

TEST(Graphics, LowerQuotaEvictsAtOnce)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,f=32,s=30,v=20,i=1,C=1,q=2", base64Encode(rgba(30, 20, 1, 2, 3))));
    ASSERT_EQ(t.g().images().size(), 1u);
    TerminalOptions o = t.t.options();
    o.graphicsQuota = 1000;
    t.t.setOptions(o);
    EXPECT_TRUE(t.g().images().empty());
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, ClearScreenKeepsVirtualPlacementsAndDropsImagesReachingIt)
{
    Store s(20, 5);
    s.apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 0, 0, 0)));
    s.apc("a=p,U=1,i=1,c=2,r=1,q=2");
    s.screen.cursor.y = 2;
    s.apc("a=T,f=32,s=10,v=60,i=2,C=1,q=2", base64Encode(rgba(10, 60, 0, 0, 0))); // rows 2-4
    s.screen.scrollUp(Region{0, 4, 0, 19}, 3, StyleTable::DefaultId, true); // its top row is history now
    s.apc("a=t,f=32,s=1,v=1,i=3,q=2", base64Encode(rgba(1, 1, 0, 0, 0)));
    s.screen.cursor.y = 0;
    s.apc("a=p,i=3,C=1,q=2"); // wholly in the history after the next scroll
    s.screen.scrollUp(Region{0, 4, 0, 19}, 1, StyleTable::DefaultId, true);
    ASSERT_EQ(s.g.placements().size(), 3u);
    s.g.clearPlacements(s.screen, 10, 20);
    ASSERT_EQ(s.g.placements().size(), 2u);
    EXPECT_TRUE(std::any_of(s.g.placements().begin(), s.g.placements().end(), [](const Placement& p) { return p.isVirtual; }));
    EXPECT_TRUE(std::none_of(s.g.placements().begin(), s.g.placements().end(), [](const Placement& p) { return p.imageId == 2; }));
}

TEST(Graphics, DeleteVisibleIncludesImagesReachingTheScreen)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,f=32,s=10,v=200,i=1,q=2", base64Encode(rgba(10, 200, 0, 0, 0)))); // ten rows from row 0
    t.t.feed("\n\n\n\n\n"); // its top five rows are history
    ASSERT_EQ(t.g().visible(t.t.screen(), 10, 20).size(), 1u);
    t.t.feed(apc("a=d,d=a"));
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, RangeDeleteReachesHighIds)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,q=2,i=3000000000", base64Encode("abc")));
    ASSERT_EQ(t.g().images().count(3000000000u), 1u);
    t.t.feed(apc("a=d,d=R,x=1,y=4294967295"));
    EXPECT_EQ(t.g().images().count(3000000000u), 0u);
}

TEST(Graphics, RepliesToNumberedCommandsNameTheImage)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,I=13,q=2", base64Encode("abc")));
    ASSERT_EQ(t.g().images().size(), 1u);
    const std::uint32_t id = t.g().images().begin()->first;
    t.t.feed(apc("a=p,I=13,C=1"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=" + std::to_string(id) + ",I=13;OK\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=p,I=14"));
    EXPECT_EQ(t.host.written, "\x1b_GI=14;ENOENT:no such image\x1b\\");
}

TEST(Graphics, FrameEditWithoutZKeepsTheGap)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,i=1,f=24,s=1,v=1,q=2", "AAAA"));
    t.t.feed(apc("a=f,i=1,f=24,s=1,v=1,z=500,q=2", "AAAA"));
    t.t.feed(apc("a=f,i=1,r=2,f=24,s=1,v=1,q=2", "/wAA")); // new pixels for frame 2
    const Image& img = t.g().images().at(1);
    ASSERT_EQ(img.frames.size(), 2u);
    EXPECT_EQ(img.frames[1]->rgba[0], 255);
    EXPECT_EQ(img.gaps[1], 500u);
}

namespace {
/// A two-frame image (gaps 40 and 100 ms), playing in a loop.
void animated(Term& t, bool placed)
{
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=7,q=2", base64Encode(rgba(1, 1, 255, 0, 0))));
    t.t.feed(apc("a=f,f=32,s=1,v=1,i=7,z=100,q=2", base64Encode(rgba(1, 1, 0, 255, 0))));
    if (placed) t.t.feed(apc("a=p,i=7,C=1,q=2"));
    t.t.feed(apc("a=a,i=7,r=1,z=40,s=3,v=1,q=2"));
}
} // namespace

TEST(Graphics, LateTickKeepsTheAnimationRunning)
{
    Term t(20, 10);
    animated(t, true);
    t.t.advanceAnimations(1000); // starts the clock
    // More than a whole cycle (140 ms) late, less than a second.
    const std::uint64_t next = t.t.advanceAnimations(1400);
    EXPECT_GT(next, 1400u);
    EXPECT_LE(next, 1500u);
}

TEST(Graphics, AnimationOfAnImageNotShownAsksForNoFrames)
{
    Term t(20, 10);
    animated(t, false);
    EXPECT_EQ(t.t.advanceAnimations(1000), 0u);
    EXPECT_EQ(t.t.advanceAnimations(2000), 0u);
    EXPECT_EQ(t.g().images().at(7).currentFrame, 0u);
    t.t.feed(apc("a=p,i=7,C=1,q=2"));
    EXPECT_NE(t.t.advanceAnimations(3000), 0u);
}

TEST(Graphics, FrameLimitIsCheckedBeforeDecoding)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=1,q=2", base64Encode("abc")));
    for (std::size_t i = 1; i < GraphicsStore::MaxFrames; ++i) {
        t.t.feed(apc("a=f,f=24,s=1,v=1,i=1,q=2", base64Encode("abc")));
    }
    ASSERT_EQ(t.g().images().at(1).frames.size(), GraphicsStore::MaxFrames);
    t.host.written.clear();
    t.t.feed(apc("a=f,f=24,s=1,v=1,i=1", "AA")); // too short: decoding would say ENODATA
    // r= names the frame the command would have added.
    EXPECT_EQ(t.host.written, "\x1b_Gi=1,r=" + std::to_string(GraphicsStore::MaxFrames + 1)
                                  + ";ENOSPC:too many image frames\x1b\\");
}

TEST(Graphics, SourceRectangleIsClippedToTheImage)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=4,v=4,i=1,q=2", base64Encode(rgba(4, 4, 1, 2, 3))));
    t.t.feed(apc("a=p,i=1,p=1,x=10,c=2,r=2,C=1,q=2")); // starts past the edge: nothing to show
    t.t.feed(apc("a=p,i=1,p=2,x=2,w=10,C=1,q=2")); // wider than what is left
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].srcX, 2);
    EXPECT_EQ(vis[0].srcW, 2);
    EXPECT_EQ(vis[0].width, 2);
}

TEST(Graphics, VirtualGridSideLeftOutIsTheImageSize)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=200,v=200,i=1,q=2", base64Encode(rgba(200, 200, 1, 2, 3))));
    // Two columns, and the image's own 10 rows (200 px at 20 px), as in
    // kitty: the 20 px square sits in the middle of a 20 x 200 px grid.
    t.t.feed(apc("a=p,U=1,i=1,c=2,q=2"));
    EXPECT_FALSE(t.g().placeholderSlice(1, 0, 0, 0, 0, 0, 10, 20).has_value());
    const auto slice = t.g().placeholderSlice(1, 0, 4, 0, 0, 0, 10, 20);
    ASSERT_TRUE(slice.has_value());
    EXPECT_EQ(slice->y, 10); // the square starts 90 px down the grid
    EXPECT_EQ(slice->height, 10);
    EXPECT_EQ(slice->z, -1); // under the text and the cursor
    EXPECT_TRUE(t.g().placeholderSlice(1, 0, 5, 1, 0, 0, 10, 20).has_value());
    EXPECT_FALSE(t.g().placeholderSlice(1, 0, 6, 0, 0, 0, 10, 20).has_value());
    EXPECT_FALSE(t.g().placeholderSlice(1, 0, 10, 0, 0, 0, 10, 20).has_value()); // outside the grid
}

TEST(Graphics, UndisplayedVirtualPlacementSurvivesRowDelete)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=1,q=2", "AAAA"));
    t.t.feed(apc("a=p,U=1,i=1,c=4,r=3,q=2"));
    t.t.feed(apc("a=d,d=y,y=1"));
    EXPECT_EQ(t.g().placements().size(), 1u);
}

TEST(Graphics, DeleteDoesNotDependOnTheScrolledBackView)
{
    Term t(20, 10);
    t.t.feed(std::string(30, '\n'));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=2,q=2", base64Encode(rgba(1, 1, 3, 2, 1))));
    t.t.feed(apc("a=p,i=1,p=1,U=1,c=1,r=1,q=2"));
    t.t.feed("\x1b[10;4H\x1b[38;2;0;0;1m\xf4\x8e\xbb\xae\x1b[0m");
    t.t.feed(apc("a=p,i=2,p=1,P=1,Q=1,q=2")); // a child on the placeholder's cell
    ASSERT_EQ(t.g().placements().size(), 2u);
    t.t.scrollViewport(10); // the placeholder's row is below the view now
    ASSERT_EQ(t.t.screen().viewportOffset(), 10u);
    t.t.feed(apc("a=d,d=p,x=4,y=10"));
    ASSERT_EQ(t.g().placements().size(), 1u);
    EXPECT_TRUE(t.g().placements()[0].isVirtual);
}

TEST(Graphics, SharedMemoryFifoDoesNotHang)
{
    if (!std::filesystem::is_directory("/dev/shm")) {
        GTEST_SKIP() << "no /dev/shm";
    }
    const ShmName shm{"/afterglow-graphics-fifo-" + std::to_string(getpid())};
    ASSERT_EQ(mkfifo(("/dev/shm" + shm.name).c_str(), 0600), 0);
    Term t(20, 10);
    t.t.feed(apc("a=q,i=1,t=s,f=24,s=1,v=1", base64Encode(shm.name)));
    EXPECT_NE(t.host.written.find("EBADF"), std::string::npos);
}

TEST(Graphics, FileTransmissionCanBeTurnedOff)
{
    TempFile file("afterglow-tty-graphics-protocol-");
    ASSERT_TRUE(file.write("RGBA"));
    Store s(20, 5);
    s.g.setFileTransfer(false);
    s.apc("a=t,t=t,f=32,s=1,v=1,i=1", base64Encode(file.path));
    EXPECT_EQ(s.host.written, "\x1b_Gi=1;EPERM:file transmission is turned off\x1b\\");
    EXPECT_TRUE(std::filesystem::exists(file.path)); // nothing opened or deleted
    s.host.written.clear();
    s.g.setFileTransfer(true);
    s.apc("a=t,t=t,f=32,s=1,v=1,i=1", base64Encode(file.path));
    EXPECT_EQ(s.host.written, "\x1b_Gi=1;OK\x1b\\");
    EXPECT_FALSE(std::filesystem::exists(file.path)); // a temporary file is the terminal's to delete
}

TEST(Graphics, TerminalOptionTurnsFileTransmissionOff)
{
    TempFile file("afterglow-tty-graphics-protocol-option-");
    ASSERT_TRUE(file.write("RGBA"));
    TerminalOptions off;
    off.graphicsFileTransfer = false;
    Term t(20, 10, off);
    t.t.feed(apc("a=t,t=t,f=32,s=1,v=1,i=1", base64Encode(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;EPERM:file transmission is turned off\x1b\\");
    EXPECT_TRUE(std::filesystem::exists(file.path));
    // A reload that allows it again reaches the store.
    t.host.written.clear();
    t.t.setOptions(TerminalOptions{});
    t.t.feed(apc("a=t,t=t,f=32,s=1,v=1,i=1", base64Encode(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;OK\x1b\\");
    // And one that turns it off again, on the alternate screen too.
    ASSERT_TRUE(file.write("RGBA"));
    t.host.written.clear();
    t.t.setOptions(off);
    t.t.feed("\x1b[?1049h");
    t.t.feed(apc("a=t,t=t,f=32,s=1,v=1,i=2", base64Encode(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;EPERM:file transmission is turned off\x1b\\");
}

TEST(Graphics, RefusedCommandStillDeletesItsTemporaryFile)
{
    TempFile file("afterglow-tty-graphics-protocol-");
    ASSERT_TRUE(file.write("RGBA"));
    Term t(20, 10);
    t.t.feed(apc("a=f,i=99,t=t,f=32,s=1,v=1", base64Encode(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=99;ENOENT:no such image\x1b\\");
    EXPECT_FALSE(std::filesystem::exists(file.path));
}

TEST(Graphics, CommandRefusedBeforeReadingDeletesItsTemporaryFile)
{
    // Each of these is refused for what it declares, before the file is
    // read: the file is still the terminal's to delete.
    TerminalOptions small;
    small.graphicsQuota = 4096;
    for (std::string_view keys : {"a=t,t=t,f=32,s=1,v=1,o=x,i=1", // unknown compression
                                  "a=t,t=t,f=99,s=1,v=1,i=1", // unknown format
                                  "a=t,t=t,f=32,v=1,i=1", // no width
                                  "a=t,t=t,f=32,s=10000,v=10000,i=1", // past the payload limit
                                  "a=t,t=t,f=32,s=40,v=40,i=1", // past the quota
                                  "a=x,t=t,f=32,s=1,v=1,i=1"}) { // unknown action
        TempFile file("afterglow-tty-graphics-protocol-");
        ASSERT_TRUE(file.write("RGBA"));
        Term t(20, 10, small);
        t.t.feed(apc(keys, base64Encode(file.path)));
        EXPECT_NE(t.host.written.find("i=1;E"), std::string::npos) << keys << ": " << t.host.written;
        EXPECT_FALSE(std::filesystem::exists(file.path)) << keys;
    }
}

TEST(Graphics, IdWithNumberStillDeletesItsTemporaryMedium)
{
    // Refused before its payload is read, like the commands above: the
    // temporary file and the tagged shared memory object still go.
    TempFile file("afterglow-tty-graphics-protocol-");
    ASSERT_TRUE(file.write("RGBA"));
    const ShmName shm{"/afterglow-tty-graphics-protocol-both-" + std::to_string(getpid())};
    const int fd = shm_open(shm.name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(write(fd, "RGBA", 4), 4);
    close(fd);
    Term t(20, 10);
    t.t.feed(apc("a=t,t=t,f=32,s=1,v=1,i=1,I=2", base64Encode(file.path)));
    t.t.feed(apc("a=t,t=s,f=32,s=1,v=1,i=3,I=4", base64Encode(shm.name)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1,I=2;EINVAL:both an image id and an image number\x1b\\"
                              "\x1b_Gi=3,I=4;EINVAL:both an image id and an image number\x1b\\");
    EXPECT_FALSE(std::filesystem::exists(file.path));
    EXPECT_EQ(shm_open(shm.name.c_str(), O_RDONLY, 0), -1);
    EXPECT_TRUE(t.g().images().empty());
}

TEST(Graphics, ReadOverTheWorkBudgetIsBusy)
{
    // A budget of one 1x1 image with no refill: the second read is refused
    // as busy, not as a failure to read. The temporary file still goes, so
    // the reply does not ask for a retry with it.
    Store s(20, 5);
    s.g.setWorkBudget(4, 0);
    TempFile first("afterglow-tty-graphics-protocol-");
    TempFile second("afterglow-tty-graphics-protocol-");
    ASSERT_TRUE(first.write("RGBA"));
    ASSERT_TRUE(second.write("RGBA"));
    s.apc("a=t,t=t,f=32,s=1,v=1,i=1", base64Encode(first.path));
    s.apc("a=t,t=t,f=32,s=1,v=1,i=2", base64Encode(second.path));
    EXPECT_EQ(s.host.written, "\x1b_Gi=1;OK\x1b\\\x1b_Gi=2;EBUSY:too much image data read\x1b\\");
    EXPECT_EQ(s.g.images().count(2), 0u);
    EXPECT_FALSE(std::filesystem::exists(second.path));
    // A t=f file stays, and a retry can read it when the budget fills again.
    TempFile kept("afterglow-graphics-busy-");
    ASSERT_TRUE(kept.write("RGBA"));
    s.host.written.clear();
    s.apc("a=t,t=f,f=32,s=1,v=1,i=4", base64Encode(kept.path));
    EXPECT_EQ(s.host.written, "\x1b_Gi=4;EBUSY:too much image data read, try again later\x1b\\");
    EXPECT_TRUE(std::filesystem::exists(kept.path));
    // A t=t file without the tag is not the terminal's to delete: it stays,
    // and the reply asks for a retry.
    TempFile untagged("afterglow-graphics-busy-");
    ASSERT_TRUE(untagged.write("RGBA"));
    s.host.written.clear();
    s.apc("a=t,t=t,f=32,s=1,v=1,i=5", base64Encode(untagged.path));
    EXPECT_EQ(s.host.written, "\x1b_Gi=5;EBUSY:too much image data read, try again later\x1b\\");
    EXPECT_TRUE(std::filesystem::exists(untagged.path));
    // A file read and a shared memory read draw on the same budget. A tagged
    // object goes with the refusal, and an untagged one stays.
    const auto object = [](const std::string& name) {
        const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        const bool written = fd >= 0 && write(fd, "RGBA", 4) == 4;
        if (fd >= 0) close(fd);
        return written;
    };
    const auto exists = [](const std::string& name) {
        const int fd = shm_open(name.c_str(), O_RDONLY, 0);
        if (fd >= 0) close(fd);
        return fd >= 0;
    };
    const ShmName tagged{"/afterglow-tty-graphics-protocol-busy-" + std::to_string(getpid())};
    ASSERT_TRUE(object(tagged.name));
    s.host.written.clear();
    s.apc("a=t,t=s,f=32,s=1,v=1,i=3", base64Encode(tagged.name));
    EXPECT_EQ(s.host.written, "\x1b_Gi=3;EBUSY:too much image data read\x1b\\");
    EXPECT_FALSE(exists(tagged.name));
    const ShmName plain{"/afterglow-graphics-busy-" + std::to_string(getpid())};
    ASSERT_TRUE(object(plain.name));
    s.host.written.clear();
    s.apc("a=t,t=s,f=32,s=1,v=1,i=6", base64Encode(plain.name));
    EXPECT_EQ(s.host.written, "\x1b_Gi=6;EBUSY:too much image data read, try again later\x1b\\");
    EXPECT_TRUE(exists(plain.name));
}

TEST(Graphics, FrameEditOverTheWorkBudgetIsBusy)
{
    // Frame edits draw on the work budget too. The reply asks for a retry
    // only when the command can be sent again: a temporary file goes.
    Store s(20, 5);
    s.apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 255, 0, 0)));
    s.apc("a=f,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 0, 0, 255)));
    ASSERT_EQ(s.g.images().at(1).frames.size(), 2u);
    s.g.setWorkBudget(0, 0);
    s.host.written.clear();
    s.apc("a=f,f=32,s=1,v=1,i=1", base64Encode(rgba(1, 1, 0, 255, 0)));
    EXPECT_EQ(s.host.written, "\x1b_Gi=1,r=3;EBUSY:too much frame editing, try again later\x1b\\");
    s.host.written.clear();
    s.apc("a=c,i=1,r=1,c=2");
    EXPECT_EQ(s.host.written, "\x1b_Gi=1;EBUSY:too much frame editing, try again later\x1b\\");
    TempFile file("afterglow-tty-graphics-protocol-");
    ASSERT_TRUE(file.write("RGBA"));
    s.host.written.clear();
    s.apc("a=f,t=t,f=32,s=1,v=1,i=1", base64Encode(file.path));
    EXPECT_EQ(s.host.written, "\x1b_Gi=1,r=3;EBUSY:too much frame editing\x1b\\");
    EXPECT_FALSE(std::filesystem::exists(file.path));
    EXPECT_EQ(s.g.images().at(1).frames.size(), 2u);
}

TEST(Graphics, RefusedCommandDeletesATemporaryFileByItsRealName)
{
    // A link whose own name lacks the tag, to a temporary file that has it:
    // a refused command deletes the file, as an accepted one does.
    for (std::string_view keys : {"a=t,t=t,f=32,s=1,v=1,o=x,i=1", "a=t,t=t,f=32,s=1,v=1,i=1"}) {
        TempFile file("afterglow-tty-graphics-protocol-");
        ASSERT_TRUE(file.write("RGBA"));
        TempFile link("afterglow-graphics-link-");
        ASSERT_EQ(unlink(link.path.c_str()), 0);
        ASSERT_EQ(symlink(file.path.c_str(), link.path.c_str()), 0);
        Term t(20, 10);
        t.t.feed(apc(keys, base64Encode(link.path)));
        EXPECT_FALSE(std::filesystem::exists(file.path)) << keys << ": " << t.host.written;
    }
}

TEST(Graphics, RawFileIsReadOnlyAsFarAsTheImage)
{
    // A sparse file past the payload limit: a 1x1 image reads its first
    // four bytes and nothing more.
    TempFile file("afterglow-graphics-raw-");
    ASSERT_TRUE(file.write("RGBA"));
    ASSERT_EQ(truncate(file.path.c_str(), off_t(GraphicsStore::DefaultQuota) + 4096), 0);
    Term t(20, 10);
    t.t.feed(apc("a=t,t=f,f=32,s=1,v=1,i=1", base64Encode(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;OK\x1b\\");
    ASSERT_EQ(t.g().images().count(1), 1u);
    const auto& pixels = t.g().images().at(1).frames[0]->rgba;
    EXPECT_EQ(std::string(pixels.begin(), pixels.end()), "RGBA");
    // S past the limit asks for no more than the image either.
    t.host.written.clear();
    t.t.feed(apc("a=t,t=f,f=32,s=1,v=1,i=2,S=4294967295", base64Encode(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;OK\x1b\\");
}

TEST(Graphics, EncodedInputHasTheInlineLimitInEveryMedium)
{
    // The quota bounds the decoded image, not the PNG or o=z bytes: input
    // longer than the quota whose pixels fit is kept, from a file as inline.
    const std::string png(8192, 'x');
    TempFile pngFile("afterglow-graphics-big-");
    ASSERT_TRUE(pngFile.write(png));
    PngHost host; // any bytes decode to 2x2 pixels, 16 bytes
    TerminalOptions small;
    small.graphicsQuota = 4096;
    Terminal t(20, 10, host, small);
    t.resize(20, 10, 10, 20);
    t.feed(apc("a=t,t=f,f=100,i=1", base64Encode(pngFile.path)));
    EXPECT_EQ(host.written, "\x1b_Gi=1;OK\x1b\\");
    host.written.clear();
    t.feed(apc("a=t,f=100,i=2", base64Encode(png)));
    EXPECT_EQ(host.written, "\x1b_Gi=2;OK\x1b\\");

    // Pixels past the quota are refused the same way in both mediums.
    host.width = 64;
    host.height = 32;
    host.bytes = 64 * 32 * 4;
    host.written.clear();
    t.feed(apc("a=t,t=f,f=100,i=3", base64Encode(pngFile.path)));
    EXPECT_EQ(host.written, "\x1b_Gi=3;ENOSPC:image larger than the storage quota\x1b\\");
    host.written.clear();
    t.feed(apc("a=t,f=100,i=4", base64Encode(png)));
    EXPECT_EQ(host.written, "\x1b_Gi=4;ENOSPC:image larger than the storage quota\x1b\\");

    // Noise does not compress: o=z data for a 32x32 image (4,096 bytes, the
    // quota) is longer than the quota and still inflates to an image that fits.
    std::string noise(32 * 32 * 4, '\0');
    std::uint32_t state = 2463534242u;
    for (char& c : noise) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        c = char(state);
    }
    const std::string deflated = zlibCompress(noise);
    ASSERT_GT(deflated.size(), small.graphicsQuota);
    TempFile zFile("afterglow-graphics-z-");
    ASSERT_TRUE(zFile.write(deflated));
    host.written.clear();
    t.feed(apc("a=t,t=f,f=32,s=32,v=32,o=z,i=5", base64Encode(zFile.path)));
    EXPECT_EQ(host.written, "\x1b_Gi=5;OK\x1b\\");
    host.written.clear();
    t.feed(apc("a=t,f=32,s=32,v=32,o=z,i=6", base64Encode(deflated)));
    EXPECT_EQ(host.written, "\x1b_Gi=6;OK\x1b\\");
}

TEST(Graphics, PlacementLimitEvictsTheOldestInternalImage)
{
    Term t(20, 10);
    t.t.feed("\x1bPq#1~\x1b\\");
    ASSERT_EQ(t.g().placements().size(), 1u);
    const std::uint32_t first = t.g().placements()[0].imageId;
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=1,q=2", base64Encode("abc")));
    for (std::size_t i = 1; i < GraphicsStore::MaxPlacements; ++i) {
        t.t.feed(apc("a=p,i=1,C=1,q=2,p=" + std::to_string(i)));
    }
    ASSERT_EQ(t.g().placements().size(), GraphicsStore::MaxPlacements);
    t.t.feed("\x1b[5;5H\x1bPq#1~\x1b\\");
    EXPECT_EQ(t.g().placements().size(), GraphicsStore::MaxPlacements);
    EXPECT_EQ(t.g().images().count(first), 0u);
    const auto kitty = std::count_if(t.g().placements().begin(), t.g().placements().end(),
                                     [](const Placement& p) { return p.imageId == 1; });
    EXPECT_EQ(std::size_t(kitty), GraphicsStore::MaxPlacements - 1);
}

TEST(Graphics, ReflowMovesAnImageToItsCharacterColumn)
{
    Store s(10, 6);
    for (int x = 0; x < 10; ++x) {
        s.screen.writeCell(s.screen.row(0), x, U'0' + char32_t(x), StyleTable::DefaultId, 1);
    }
    s.screen.row(0).wrapped = true;
    s.screen.writeCell(s.screen.row(1), 0, U'A', StyleTable::DefaultId, 1);
    s.screen.writeCell(s.screen.row(1), 1, U'B', StyleTable::DefaultId, 1);
    s.screen.cursor.x = 2;
    s.screen.cursor.y = 1;
    s.apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 0, 0, 0)));
    s.apc("a=p,i=1,C=1,q=2"); // right after "AB"
    auto rows = s.g.anchorRows(s.screen);
    auto columns = s.g.anchorColumns();
    s.screen.resize(20, 6, true, std::nullopt, &rows, &columns);
    s.g.retarget(s.screen, rows, &columns);
    ASSERT_EQ(s.g.placements().size(), 1u);
    EXPECT_EQ(s.g.placements()[0].col, 12);
    EXPECT_EQ(s.screen.absoluteIndexOf(s.g.placements()[0].row), 0);
}

TEST(Graphics, InternalImageIsDrawnAtItsDisplaySize)
{
    Store s(20, 10);
    ImageFrame frame;
    frame.width = 2;
    frame.height = 1;
    frame.rgba.assign(8, 255);
    s.g.addSixel(std::move(frame), s.screen, 0, 0, 10, 20, 40, 20);
    const auto vis = s.g.visible(s.screen, 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].width, 40);
    EXPECT_EQ(vis[0].height, 20);
    EXPECT_EQ(vis[0].srcW, 2);
    EXPECT_EQ(vis[0].frame->width, 2u);
}

TEST(ITerm2, CappedSizeKeepsTheShape)
{
    detail::ITerm2File file;
    file.width = "100%";
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    // 1 x 100 px at the full 2,000 px width would be 200,000 px high.
    ASSERT_TRUE(detail::iterm2ImageSize(file, 1, 100, 10, 20, 200, 50, w, h));
    EXPECT_EQ(w, 100u);
    EXPECT_EQ(h, 10000u);
}

TEST(ITerm2, ScaleFrameFiltersEachAxisOnItsOwn)
{
    ImageFrame in;
    in.width = 2;
    in.height = 2;
    in.rgba = {255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255}; // red row over a blue row
    // Narrower (averaged) and taller (interpolated): the rows blend smoothly.
    const ImageFrame out = detail::scaleFrame(in, 1, 4);
    ASSERT_EQ(out.rgba.size(), 16u);
    EXPECT_EQ(out.rgba[0], 255);
    EXPECT_NEAR(out.rgba[4], 191, 2);
    EXPECT_NEAR(out.rgba[6], 64, 2);
    EXPECT_NEAR(out.rgba[8], 64, 2);
    EXPECT_EQ(out.rgba[14], 255);
    EXPECT_EQ(out.rgba[7], 255);
}

TEST(Graphics, ClientWriterOutputIsStored)
{
    // What a client writes with the protocol library: zlib data in several chunks.
    std::string raw;
    std::uint32_t seed = 5;
    for (int i = 0; i < 64 * 64 * 4; ++i) {
        seed = seed * 1664525u + 1013904223u;
        raw += char(seed >> 24);
    }
    const auto cmd = AfterglowProto::GraphicsCommand::parse("a=t,f=32,s=64,v=64,o=z,i=6");
    const std::string stream = AfterglowProto::writeGraphics(cmd, AfterglowProto::zlibCompress(raw));
    ASSERT_NE(stream.find("\x1b_Gm=1;"), std::string::npos);
    Term t(20, 10);
    t.t.feed(stream);
    EXPECT_EQ(t.host.written, "\x1b_Gi=6;OK\x1b\\");
    ASSERT_EQ(t.g().images().count(6), 1u);
    const auto& pixels = t.g().images().at(6).frames[0]->rgba;
    EXPECT_EQ(std::string(pixels.begin(), pixels.end()), raw);
}

TEST(Graphics, PlacementsWithoutAnIdNeverReplaceOnesWithIt)
{
    // Ids the store chooses come from the range a client may name: the
    // ones in use are skipped.
    Term t(40, 20);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 1, 2, 3))));
    const std::uint32_t first = 1u << 31;
    t.t.feed(apc("a=p,i=1,C=1,q=2,p=" + std::to_string(first)));
    t.t.feed(apc("a=p,i=1,C=1,q=2,p=" + std::to_string(first + 1)));
    t.t.feed(apc("a=p,i=1,C=1,q=2"));
    t.t.feed(apc("a=p,i=1,C=1,q=2"));
    const auto& placements = t.g().placements();
    ASSERT_EQ(placements.size(), 4u);
    std::vector<std::uint32_t> ids;
    for (const auto& p : placements) ids.push_back(p.placementId);
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
    EXPECT_EQ(std::count(ids.begin(), ids.end(), 0u), 0);
}

TEST(Graphics, LastChunkPrunesAndSettles)
{
    // A chunk before the last only buffers; the last still drops placements
    // whose rows left the scrollback while the upload ran, and anonymous
    // images nothing shows.
    Store s(20, 5, 2);
    s.apc("a=t,f=32,s=1,v=1,i=1,q=2", base64Encode(rgba(1, 1, 0, 0, 0)));
    s.apc("a=p,i=1,C=1,q=2"); // row 0
    const std::string data = base64Encode(rgba(2, 2, 9, 8, 7));
    s.apc("a=t,f=32,s=2,v=2,q=2,m=1", data.substr(0, 8)); // anonymous
    s.screen.scrollUp(Region{0, 4, 0, 19}, 5, StyleTable::DefaultId, true); // row 0 leaves the two rows of scrollback
    s.apc("m=1", data.substr(8, 8));
    s.apc("m=0", data.substr(16));
    EXPECT_TRUE(s.g.placements().empty());
    EXPECT_EQ(s.g.images().size(), 1u); // image 1 has an id and stays; the anonymous one goes
    EXPECT_EQ(s.g.images().count(1), 1u);
}

TEST(Graphics, PlaceholderContinuesOnlyWhatAgrees)
{
    // kitty's rule: a cell continues the run on its left only when every
    // diacritic it has agrees with it.
    Term t(20, 10);
    t.t.feed("\x1b[38;2;0;0;1m");
    // (a) A row that differs starts a new run at column 0.
    t.t.feed(placeholder({0, 0}) + placeholder({1}) + "\r\n");
    // (b) A high byte that differs starts a new run, whose column is 0.
    t.t.feed(placeholder({0, 0, 1}) + placeholder({0, -1, 2}) + "\r\n");
    // (c) A column that is not the next starts a new run at row 0; a cell
    // that agrees takes the rest from the run.
    t.t.feed(placeholder({3, 0}) + placeholder({-1, 5}) + placeholder({}) + "\x1b[0m");
    const Screen& screen = t.t.screen();
    auto decode = [&](int y) {
        PlaceholderDecoder decoder;
        std::vector<PlaceholderCell> cells;
        for (int x = 0; x < screen.cols(); ++x) {
            if (const auto cell = decoder.next(screen, screen.row(y), x)) cells.push_back(*cell);
        }
        return cells;
    };
    const auto a = decode(0);
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[1].row, 1);
    EXPECT_EQ(a[1].col, 0);
    const auto b = decode(1);
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0].imageId, 0x01000001u);
    EXPECT_EQ(b[1].imageId, 0x02000001u);
    EXPECT_EQ(b[1].col, 0);
    const auto c = decode(2);
    ASSERT_EQ(c.size(), 3u);
    EXPECT_EQ(c[1].row, 0);
    EXPECT_EQ(c[1].col, 5);
    EXPECT_EQ(c[2].row, 0);
    EXPECT_EQ(c[2].col, 6);
}

TEST(Graphics, AlternateScreenClearDropsItsImagesAndUploads)
{
    Term t(20, 10);
    t.t.feed("\x1b[?1049h");
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=5,q=2", base64Encode(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=6,m=1,q=2", "AAAA")); // an upload left unfinished
    ASSERT_EQ(t.g().images().count(5), 1u);
    t.t.feed("\x1b[?1049l\x1b[?1049h");
    EXPECT_TRUE(t.g().images().empty());
    t.t.feed(apc("a=p,i=5"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=5;ENOENT:no such image\x1b\\");
    // A stray last chunk does not finish the old program's upload.
    t.t.feed(apc("m=0", "AAAA"));
    EXPECT_EQ(t.g().images().count(6), 0u);
}

TEST(Graphics, AnimationOutOfViewAsksForNoFrames)
{
    Term t(20, 10);
    animated(t, true);
    EXPECT_NE(t.t.advanceAnimations(1000), 0u);
    t.t.feed(std::string(30, '\n')); // the image's row goes into the scrollback
    EXPECT_EQ(t.t.advanceAnimations(2000), 0u);
    t.t.scrollViewport(30); // and back into view
    EXPECT_NE(t.t.advanceAnimations(3000), 0u);
}

TEST(Graphics, VirtualAnimationRunsOnlyWhileItsPlaceholdersShow)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=7,q=2", base64Encode(rgba(1, 1, 255, 0, 0))));
    t.t.feed(apc("a=f,f=32,s=1,v=1,i=7,z=100,q=2", base64Encode(rgba(1, 1, 0, 255, 0))));
    t.t.feed(apc("a=p,i=7,U=1,c=1,r=1,q=2"));
    t.t.feed(apc("a=a,i=7,r=1,z=40,s=3,v=1,q=2"));
    EXPECT_EQ(t.t.advanceAnimations(1000), 0u);
    t.t.feed("\x1b[38;2;0;0;7m" + placeholder({0, 0}) + "\x1b[0m");
    EXPECT_NE(t.t.advanceAnimations(2000), 0u);
}

TEST(Graphics, AnimationStateKeepsARunningClock)
{
    Term t(20, 10);
    animated(t, true);
    t.t.advanceAnimations(1000); // starts the clock: frame 1 is due at 1040
    t.t.advanceAnimations(1030);
    t.t.feed(apc("a=a,i=7,s=3,q=2")); // already running
    t.t.advanceAnimations(1041);
    EXPECT_EQ(t.g().images().at(7).currentFrame, 1u);
}

TEST(Graphics, AnimationStateResetsTheLoopCount)
{
    Term t(20, 10);
    animated(t, true);
    t.t.feed(apc("a=a,i=7,v=3,q=2")); // two loops: 0 (40 ms), 1 (100 ms), 0, 1
    for (std::uint64_t now = 1000; now <= 1280; now += 10) t.t.advanceAnimations(now);
    ASSERT_EQ(t.g().images().at(7).currentFrame, 1u); // done at 1280, on the last frame
    EXPECT_EQ(t.t.advanceAnimations(1285), 0u);
    t.t.feed(apc("a=a,i=7,s=3,q=2"));
    t.t.advanceAnimations(1290);
    EXPECT_EQ(t.g().images().at(7).currentFrame, 0u); // it plays again
}

TEST(Graphics, FinishedAnimationGoesOnWithMoreLoops)
{
    Term t(20, 10);
    animated(t, true);
    t.t.feed(apc("a=a,i=7,v=2,q=2")); // plays once
    t.t.advanceAnimations(1000);
    t.t.advanceAnimations(1041);
    EXPECT_EQ(t.t.advanceAnimations(1141), 0u);
    ASSERT_EQ(t.g().images().at(7).currentFrame, 1u);
    EXPECT_EQ(t.g().images().at(7).animation, Image::Animation::Running);
    t.t.feed(apc("a=a,i=7,v=4,q=2")); // three loops: two more to play
    t.t.advanceAnimations(1150);
    EXPECT_EQ(t.g().images().at(7).currentFrame, 0u);
}

TEST(Graphics, IdWithNumberIsRefused)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=3,I=4", base64Encode("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=3,I=4;EINVAL:both an image id and an image number\x1b\\");
    EXPECT_TRUE(t.g().images().empty());
}

TEST(Graphics, AnonymousImageCannotBeNamedByItsId)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,f=24,s=1,v=1,C=1", base64Encode("abc")));
    ASSERT_EQ(t.g().placements().size(), 1u);
    const std::uint32_t id = t.g().placements()[0].imageId;
    const std::string ids = std::to_string(id);
    t.t.feed(apc("a=p,i=" + ids));
    EXPECT_EQ(t.host.written, "\x1b_Gi=" + ids + ";ENOENT:no such image\x1b\\");
    t.t.feed(apc("a=d,d=I,i=" + ids));
    t.t.feed(apc("a=d,d=R,x=" + ids + ",y=" + ids));
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=1,q=2", base64Encode("abc")));
    t.host.written.clear();
    t.t.feed(apc("a=p,i=1,P=" + ids));
    EXPECT_NE(t.host.written.find("ENOPARENT"), std::string::npos);
    ASSERT_EQ(t.g().placements().size(), 1u);
    // A client that claims the id gets its own image; the anonymous one stays.
    t.t.feed(apc("a=t,f=24,s=1,v=1,q=2,i=" + ids, base64Encode("xyz")));
    ASSERT_EQ(t.g().placements().size(), 1u);
    EXPECT_NE(t.g().placements()[0].imageId, id);
    EXPECT_EQ(t.g().images().size(), 3u); // the anonymous one, image 1 and the new one
    EXPECT_EQ(t.g().images().at(id).frames[0]->rgba[0], 'x');
}

TEST(Graphics, DeleteByZKeepsVirtualPlacements)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=1,q=2", base64Encode("abc")));
    t.t.feed(apc("a=p,i=1,p=1,U=1,z=5,q=2"));
    t.t.feed(apc("a=p,i=1,p=2,C=1,z=5,q=2"));
    t.t.feed(apc("a=d,d=z,z=5"));
    ASSERT_EQ(t.g().placements().size(), 1u);
    EXPECT_TRUE(t.g().placements()[0].isVirtual);
}

TEST(Graphics, LastQuietOfTheChunksDecides)
{
    Term t(20, 10);
    const std::string data = base64Encode(rgba(1, 1, 1, 2, 3));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=3,m=1", data.substr(0, 4)));
    t.t.feed(apc("m=0,q=1", data.substr(4)));
    EXPECT_TRUE(t.host.written.empty());
    EXPECT_EQ(t.g().images().count(3), 1u);
}

TEST(Graphics, FrameChunksFromTheClientWriterAddAFrame)
{
    // The writer gives every chunk of a frame a=f, as the protocol asks.
    std::string pixels;
    for (int i = 0; i < 40 * 40; ++i) pixels += rgba(1, 1, std::uint8_t(i), 2, 3);
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=40,v=40,i=6,q=2", base64Encode(pixels)));
    const auto frame = AfterglowProto::GraphicsCommand::parse("a=f,f=32,s=40,v=40,i=6");
    const std::string stream = AfterglowProto::writeGraphics(frame, pixels);
    ASSERT_NE(stream.find("\x1b_Ga=f,m=1;"), std::string::npos);
    t.t.feed(stream);
    EXPECT_EQ(t.host.written, "\x1b_Gi=6,r=2;OK\x1b\\"); // r= names the frame written
    EXPECT_EQ(t.g().images().at(6).frames.size(), 2u);
}

TEST(Graphics, CellOffsetsStayInsideTheCell)
{
    // A 1 px image with huge offsets: kept inside its cell, it covers one
    // cell and moves the cursor one column, not across screens.
    Term t(20, 10);
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=1,X=1000000,Y=1000000,q=2", base64Encode(rgba(1, 1, 0, 0, 0))));
    EXPECT_EQ(t.t.screen().cursor.y, 0);
    EXPECT_EQ(t.t.screen().cursor.x, 1);
    EXPECT_EQ(t.t.screen().historySize(), 0u);
}

TEST(Graphics, InternalImageLargerThanTheQuotaIsNotKept)
{
    Store s(20, 10);
    s.g.setQuota(64);
    s.apc("a=T,f=32,s=1,v=1,i=1,C=1,q=2", base64Encode(rgba(1, 1, 1, 2, 3)));
    ImageFrame big;
    big.width = 8;
    big.height = 8;
    big.rgba.assign(8 * 8 * 4, 255);
    s.g.addSixel(std::move(big), s.screen, 0, 0, 10, 20);
    EXPECT_EQ(s.g.images().size(), 1u);
    EXPECT_EQ(s.g.images().count(1), 1u);
    EXPECT_EQ(s.g.placements().size(), 1u);
}

TEST(Graphics, SameZDrawsInKittyOrder)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=1,q=2", base64Encode("abc")));
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=2,q=2", base64Encode("def")));
    t.t.feed(apc("a=p,i=2,p=1,C=1,q=2"));
    t.t.feed(apc("a=p,i=1,p=1,C=1,q=2"));
    t.t.feed(apc("a=p,i=1,p=2,C=1,q=2"));
    t.t.feed(apc("a=p,i=1,p=1,C=1,q=2")); // replaced: it keeps its place
    ASSERT_EQ(t.g().placements().size(), 3u);
    EXPECT_EQ(t.g().placements()[1].placementId, 1u);
    // At one z, the image that got its id first is drawn first.
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 3u);
    EXPECT_EQ(vis[0].imageId, 1u);
    EXPECT_EQ(vis[1].imageId, 1u);
    EXPECT_EQ(vis[2].imageId, 2u);
}

TEST(Graphics, TemporaryFileTagCountsAnywhereBelowTheDirectory)
{
    std::string dir = (std::filesystem::temp_directory_path() / "afterglow-tty-graphics-protocol-XXXXXX").string();
    ASSERT_NE(mkdtemp(dir.data()), nullptr);
    const std::string file = dir + "/image";
    {
        const int fd = open(file.c_str(), O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0600);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(write(fd, "RGBA", 4), 4);
        close(fd);
    }
    Term t(20, 10);
    t.t.feed(apc("a=t,t=t,f=32,s=1,v=1,i=1", base64Encode(file)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;OK\x1b\\");
    EXPECT_FALSE(std::filesystem::exists(file));
    std::filesystem::remove_all(dir);
}

TEST(Graphics, ShortFileGetsTheFileError)
{
    TempFile file("afterglow-graphics-short-");
    ASSERT_TRUE(file.write("RG"));
    Term t(20, 10);
    t.t.feed(apc("a=t,t=f,f=32,s=1,v=1,i=1", base64Encode(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;EBADF:failed to read image file\x1b\\");
    // Inline data that is too short says so.
    t.host.written.clear();
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=2", base64Encode("RG")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;ENODATA:insufficient image data\x1b\\");
}

TEST(Graphics, FrameNeedsDataAnExistingBaseAndToFitTheImage)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=2,v=2,i=1,q=2", base64Encode(rgba(2, 2, 1, 2, 3))));
    t.t.feed(apc("a=f,f=32,s=1,v=1,i=1,c=5", base64Encode(rgba(1, 1, 1, 2, 3))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1,r=2;EINVAL:no such base frame\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=f,f=32,s=3,v=1,i=1", base64Encode(rgba(3, 1, 1, 2, 3))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1,r=2;EINVAL:frame larger than the image\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=f,i=1"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1,r=2;ENODATA:insufficient image data\x1b\\");
    EXPECT_EQ(t.g().images().at(1).frames.size(), 1u);
}

TEST(Graphics, DeleteFramesAsKittyDoes)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=1,q=2", base64Encode("aaa")));
    t.t.feed(apc("a=f,f=24,s=1,v=1,i=1,q=2", base64Encode("bbb")));
    t.t.feed(apc("a=f,f=24,s=1,v=1,i=1,q=2", base64Encode("ccc")));
    t.t.feed(apc("a=p,i=1,C=1,q=2"));
    t.t.feed(apc("a=a,i=1,c=3,q=2")); // showing the last frame
    t.t.feed(apc("a=d,d=f,i=1,r=2"));
    const Image& img = t.g().images().at(1);
    ASSERT_EQ(img.frames.size(), 2u);
    EXPECT_EQ(img.frames[1]->rgba[0], 'c');
    EXPECT_EQ(img.currentFrame, 1u); // still the same frame
    t.t.feed(apc("a=d,d=f,i=1,r=9")); // past the last: the last
    ASSERT_EQ(t.g().images().at(1).frames.size(), 1u);
    EXPECT_EQ(t.g().images().at(1).currentFrame, 0u);
    t.t.feed(apc("a=d,d=f,i=1")); // the only frame stays with d=f
    ASSERT_EQ(t.g().images().count(1), 1u);
    t.t.feed(apc("a=d,d=F,i=1")); // and goes with d=F, with the image
    EXPECT_EQ(t.g().images().count(1), 0u);
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, FeatureQueryEchoesWhatIsSupported)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=9,m=1,q=2", "AAAA")); // an upload in progress
    t.t.feed(apc("a=q,i=31,e=T,U=1,C=1"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=31,e=T,C=1,U=1;OK\x1b\\");
    t.host.written.clear();
    // A frame takes no indexed colour (f=8): f is left out.
    t.t.feed(apc("a=q,i=32,e=f,r=1,X=1,f=8"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=32,e=f,r=1,X=1;OK\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=q,i=37,e=T,f=16"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=37,e=T,f=16;OK\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=q,i=33,e=d,d=F"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=33,e=d,d=F;OK\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=q,i=35,e=t,N=1"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=35,e=t,N=1;OK\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=q,i=36,e=f,N=1")); // a hint belongs to the transmission, not to a frame
    EXPECT_EQ(t.host.written, "\x1b_Gi=36,e=f;OK\x1b\\");
    t.host.written.clear();
    t.t.feed(apc("a=q,i=34,e=Z"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=34;OK\x1b\\"); // no such action: no e
    // The upload goes on, untouched by the queries.
    t.host.written.clear();
    t.t.feed(apc("m=0,q=2", "AA=="));
    EXPECT_EQ(t.g().images().count(9), 1u);
}
