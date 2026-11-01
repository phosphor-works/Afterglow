// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"
#include "../src/Base64.h"
#include "../src/ITerm2.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {

std::string b64(std::string_view in)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = (unsigned char)in[i] << 16;
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

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
struct TempFile
{
    std::string path;
    explicit TempFile(std::string_view stem)
        : path((std::filesystem::temp_directory_path() / (std::string(stem) + std::to_string(getpid()))).string())
    {
    }
    ~TempFile() { unlink(path.c_str()); }
};

} // namespace

TEST(Graphics, TransmitAndRespond)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=2,v=2,i=7", b64(rgba(2, 2, 255, 0, 0))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=7;OK\x1b\\");
    ASSERT_EQ(t.g().images().count(7), 1u);
    EXPECT_EQ(t.g().images().at(7).width, 2u);
    EXPECT_EQ(t.g().images().at(7).frames[0]->rgba[0], 255);
}

TEST(Graphics, QuietAndAnonymous)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=1", b64(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=t,f=32,s=1,v=1", b64(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=t,f=32,s=9,v=9,i=2,q=1", b64(rgba(1, 1, 1, 2, 3))));
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;ENODATA:insufficient image data\x1b\\");
    // The anonymous image is not kept: no command can name or show it.
    EXPECT_EQ(t.g().images().size(), 1u);
    EXPECT_EQ(t.g().images().count(1), 1u);
}

TEST(Graphics, ImageNumberGetsAnId)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,I=13", b64("abc")));
    ASSERT_EQ(t.g().images().size(), 1u);
    const std::uint32_t id = t.g().images().begin()->first;
    EXPECT_EQ(t.host.written, "\x1b_Gi=" + std::to_string(id) + ",I=13;OK\x1b\\");
}

TEST(Graphics, ChunkedTransmission)
{
    Term t(20, 10);
    const std::string data = b64(rgba(4, 4, 9, 8, 7));
    t.t.feed(apc("a=t,f=32,s=4,v=4,i=3,m=1", data.substr(0, 20)));
    t.t.feed(apc("m=1", data.substr(20, 40)));
    t.t.feed(apc("m=0", data.substr(60)));
    ASSERT_EQ(t.g().images().count(3), 1u);
    EXPECT_EQ(t.g().images().at(3).frames[0]->rgba.size(), 64u);
    EXPECT_EQ(t.host.written, "\x1b_Gi=3;OK\x1b\\");
}

TEST(Graphics, ZlibCompressed)
{
    Term t(20, 10);
    const std::string raw = rgba(8, 8, 1, 2, 3);
    uLongf len = compressBound(uLong(raw.size()));
    std::string z(len, '\0');
    compress(reinterpret_cast<Bytef*>(z.data()), &len, reinterpret_cast<const Bytef*>(raw.data()), uLong(raw.size()));
    z.resize(len);
    t.t.feed(apc("a=t,f=32,s=8,v=8,o=z,i=4", b64(z)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=4;OK\x1b\\");
    ASSERT_EQ(t.g().images().count(4), 1u);
    const auto& pixels = t.g().images().at(4).frames[0]->rgba;
    EXPECT_EQ(std::string(pixels.begin(), pixels.end()), raw);
}

TEST(Graphics, ZlibBombStopsAtThePayloadLimit)
{
    // Zeros past the payload limit compress to a few hundred KiB.
    z_stream zs{};
    ASSERT_EQ(deflateInit(&zs, Z_BEST_COMPRESSION), Z_OK);
    const std::string zeros(1 << 20, '\0');
    std::string z;
    char out[65536];
    const std::size_t blocks = GraphicsStore::DefaultQuota / zeros.size() + 2;
    for (std::size_t i = 0; i < blocks; ++i) {
        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(zeros.data()));
        zs.avail_in = uInt(zeros.size());
        const int flush = i + 1 == blocks ? Z_FINISH : Z_NO_FLUSH;
        do {
            zs.next_out = reinterpret_cast<Bytef*>(out);
            zs.avail_out = sizeof out;
            deflate(&zs, flush);
            z.append(out, sizeof out - zs.avail_out);
        } while (zs.avail_out == 0);
    }
    deflateEnd(&zs);
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,o=z,i=4", b64(z)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=4;EINVAL:zlib decompression failed\x1b\\");
    EXPECT_TRUE(t.g().images().empty());
}

TEST(Graphics, PlaceMovesCursorAndScrollsWithText)
{
    Term t(20, 10);
    // 25x45 px image at 10x20 cells: 3 cells wide, 3 rows high.
    t.t.feed("\x1b[2;3H");
    t.t.feed(apc("a=T,f=32,s=25,v=45,i=5", b64(rgba(25, 45, 0, 0, 255))));
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
    t.t.feed(apc("a=t,f=32,s=10,v=10,i=6", b64(rgba(10, 10, 0, 0, 0))));
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
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=8,q=2", b64(rgba(1, 1, 0, 0, 0))));
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
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=9,q=2", b64(rgba(1, 1, 0, 0, 0))));
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
    t.t.feed(apc("a=T,f=32,s=10,v=60,i=1,q=2", b64(rgba(10, 60, 0, 0, 0))));
    t.t.feed("\n\n\n");
    ASSERT_EQ(t.t.screen().historySize(), 5u);
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed("\x1b[2J");
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, SixelAllocationAndCoveragePreserveKittyImagesInInternalRange)
{
    Term t(20, 10);
    const auto id = 3u << 29;
    t.t.feed(apc("a=T,f=32,s=1,v=1,C=1,q=2,i=" + std::to_string(id), b64(rgba(1, 1, 255, 0, 0))));
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
    t.t.feed(apc("a=T,f=32,s=1,v=1,C=1,q=2,i=" + std::to_string(old.imageId), b64(rgba(1, 1, 255, 0, 0))));
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
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", b64(rgba(1, 1, 1, 2, 3))));
    t.host.written.clear();
    t.t.feed(apc("a=p,i=1,P=" + identity));
    EXPECT_NE(t.host.written.find("ENOPARENT"), std::string::npos);
    EXPECT_EQ(t.g().placements().size(), 1u);
}

TEST(Graphics, AltScreenHasItsOwnImages)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", b64(rgba(1, 1, 0, 0, 0))));
    t.t.feed("\x1b[?1049h");
    EXPECT_TRUE(t.g().images().empty());
    t.t.feed("\x1b[?1049l");
    EXPECT_EQ(t.g().images().size(), 1u);
}

TEST(Graphics, AlternateScreenClearRemovesPlacementsAcrossSessions)
{
    Term t(20, 10);
    t.t.feed("\x1b[?47h");
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=1,C=1,q=2", b64(rgba(1, 1, 1, 2, 3))));
    t.t.feed("\x1b[?47l\x1b[?47h");
    ASSERT_EQ(t.g().placements().size(), 1u); // mode 47 preserves the screen
    t.t.feed("\x1b[?47l\x1b[?1049h");
    EXPECT_TRUE(t.g().placements().empty()); // 1049 clears on entry
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=2,C=1,q=2", b64(rgba(1, 1, 4, 5, 6))));
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed("\x1b[?1049l\x1b[?47h");
    EXPECT_TRUE(t.g().placements().empty()); // and on exit
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=3,C=1,q=2", b64(rgba(1, 1, 7, 8, 9))));
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.feed("\x1b[?1047l\x1b[?47h");
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, FramesAndAnimation)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=2,v=1,i=10,q=2", b64(rgba(2, 1, 255, 0, 0))));
    t.t.feed(apc("a=f,f=32,s=1,v=1,i=10,x=1,z=100,q=2", b64(rgba(1, 1, 0, 255, 0))));
    t.t.feed(apc("a=p,i=10,C=1,q=2")); // only an image on screen animates
    const Image& img = t.g().images().at(10);
    ASSERT_EQ(img.frames.size(), 2u);
    // new frame: transparent canvas with the patch at x=1
    EXPECT_EQ(img.frames[1]->rgba[3], 0);
    EXPECT_EQ(img.frames[1]->rgba[5], 255);
    EXPECT_EQ(img.gaps[1], 100u);

    t.t.feed(apc("a=a,i=10,s=3,v=1,q=2"));
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
    t.t.feed(apc("a=t,t=f,f=32,s=1,v=1,i=11", b64("/dev/zero")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=11;EBADF:not a readable regular file\x1b\\");
}

TEST(Graphics, ImageStaysAnchoredWhenTheScreenGrows)
{
    // Regression: images were kept at a distance from the bottom, so growing
    // the screen (bottom padding) slid them down.
    Term t(40, 10);
    t.t.feed("\r\n\r\n");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=12,C=1,q=2", b64(rgba(10, 20, 0, 0, 0))));
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
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=13,C=1,q=2", b64(rgba(10, 20, 0, 0, 0))));
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
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=14,C=1,q=2", b64(rgba(10, 20, 0, 0, 0)))); // right after "AB"
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
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=1,C=1,q=2", b64(rgba(10, 20, 0, 0, 0)))); // row 0
    t.t.feed("\x1b[4;5H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=2,C=1,q=2", b64(rgba(10, 20, 0, 0, 0)))); // row 3, column 4
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
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=1,C=1,q=2", b64(rgba(10, 20, 0, 0, 0)))); // row 3
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
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=1,C=1,q=2", b64(rgba(10, 20, 0, 0, 0)))); // row 0, above the region
    t.t.feed("\x1b[3;1H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=2,C=1,q=2", b64(rgba(10, 20, 0, 0, 0)))); // row 2
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
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=3,C=1,q=2", b64(rgba(10, 20, 0, 0, 0)))); // row 3, column 2
    t.t.feed("\x1b[4;16H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=4,C=1,q=2", b64(rgba(10, 20, 0, 0, 0)))); // row 3, column 15
    t.t.feed("\x1b[1S");
    EXPECT_EQ(rowOf(3), 2);
    EXPECT_EQ(rowOf(4), 3);
}

TEST(Graphics, AlternateScreenRegionScrollMovesImages)
{
    Term t(20, 6);
    t.t.feed("\x1b[?1049h\x1b[2;6r\x1b[4;1H");
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=1,C=1,q=2", b64(rgba(10, 20, 0, 0, 0)))); // row 3
    t.t.feed("\x1b[6;1H\n"); // a line feed at the bottom margin
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 40);
}

TEST(Graphics, HugeRowCountDoesNotHang)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=14,q=2", b64(rgba(1, 1, 0, 0, 0))));
    t.t.feed(apc("a=p,i=14,r=2000000000,q=2"));
    EXPECT_EQ(t.t.screen().cursor.y, 9);
}

TEST(Graphics, UnicodePlaceholders)
{
    Term t(20, 10);
    // A 40x40 image as a virtual placement of 4x2 cells (10x20 px cells).
    t.t.feed(apc("a=t,f=32,s=40,v=40,i=3,q=2", b64(rgba(40, 40, 1, 2, 3))));
    t.t.feed(apc("a=p,i=3,U=1,c=4,r=2,q=2"));
    EXPECT_EQ(placeholderDiacriticValue(0x0305), 0);
    EXPECT_EQ(placeholderDiacriticValue(0x030D), 1);
    EXPECT_EQ(placeholderDiacriticValue(U'a'), -1);
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
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1", b64(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=2", b64(rgba(1, 1, 9, 8, 7))));
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
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1", b64(rgba(1, 1, 1, 2, 3))));
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
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1", b64(rgba(1, 1, 1, 2, 3))));
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
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=1,P=99", b64(rgba(1, 1, 1, 2, 3))));
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
    t.t.feed(apc("a=t,t=s,f=32,s=1,v=1,O=4,i=7", b64(shm.name)));
    ASSERT_EQ(t.g().images().count(7), 1u);
    EXPECT_EQ(t.g().images().at(7).frames[0]->rgba[0], 'R');
    EXPECT_EQ(shm_open(shm.name.c_str(), O_RDONLY, 0), -1); // decoded, so deleted
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
    t.t.feed(apc("a=t,t=f,f=32,s=1,v=1,i=7", b64(name)));
    EXPECT_TRUE(t.g().images().empty());
    EXPECT_NE(t.host.written.find("EFBIG"), std::string::npos);
    unlink(name);
    ASSERT_EQ(mkfifo(name, 0600), 0);
    t.host.written.clear();
    t.t.feed(apc("a=t,t=f,f=32,s=1,v=1,i=8", b64(name)));
    EXPECT_TRUE(t.g().images().empty());
    EXPECT_NE(t.host.written.find("EBADF"), std::string::npos);
    unlink(name);
}

TEST(Graphics, ExtendedViewportIncludesImageBelowVisibleRows)
{
    Term t(20, 3);
    t.t.feed("\n\n\n");
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=8,C=1,q=2", b64(rgba(1, 1, 0, 0, 0))));
    t.t.screen().scrollViewport(1);
    EXPECT_TRUE(t.g().visible(t.t.screen(), 10, 20).empty());
    EXPECT_EQ(t.g().visible(t.t.screen(), 10, 20, 1).size(), 1u);
}

TEST(Graphics, PlacementMetadataAndHostileGeometryAreBounded)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", b64(rgba(1, 1, 0, 0, 0))));
    for (int i = 1; i <= 4100; ++i) {
        t.t.feed(apc("a=p,i=1,C=1,q=2,p=" + std::to_string(i)));
    }
    EXPECT_EQ(t.g().placements().size(), 4096u);
    t.t.feed(apc("a=p,i=1,p=1,c=2147483647,r=2147483647,X=2147483647,Y=2147483647,C=1,q=2"));
    ASSERT_EQ(t.g().placements().size(), 4096u); // replaced, not added
    // The huge placement's offset puts it far right of any cell: it is not
    // drawn; the 4,095 one-cell placements are, with sane sizes.
    const auto vis = t.g().visible(t.t.screen(), 65535, 65535);
    EXPECT_EQ(vis.size(), 4095u);
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
    t.t.feed(apc("a=q,i=31,s=1,v=1,f=24", b64("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=31;OK\x1b\\");
    EXPECT_TRUE(t.g().images().empty());
    t.host.written.clear();
    const std::string data = b64("abcdef"); // two pixels
    t.t.feed(apc("a=q,i=32,s=2,v=1,f=24,m=1", data.substr(0, 4)));
    t.t.feed(apc("m=0", data.substr(4)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=32;OK\x1b\\");
    EXPECT_TRUE(t.g().images().empty());
}

TEST(Graphics, ComposeCopiesARectangleBetweenFrames)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=2,v=1,i=1,q=2", b64(rgba(2, 1, 255, 0, 0))));
    t.t.feed(apc("a=f,f=32,s=2,v=1,i=1,q=2", b64(rgba(2, 1, 0, 0, 255))));
    ASSERT_EQ(t.g().images().at(1).frames.size(), 2u);
    t.host.written.clear();
    // Frame 1's pixel (0, 0) onto frame 2 at (1, 0).
    t.t.feed(apc("a=c,i=1,r=1,c=2,w=1,h=1,X=1"));
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
    t.feed(apc("a=t,f=100,i=1", b64("png")));
    EXPECT_EQ(host.written, "\x1b_Gi=1;OK\x1b\\");
    host.written.clear();
    host.width = 10001;
    host.height = 1;
    host.bytes = 10001 * 4;
    t.feed(apc("a=t,f=100,i=2", b64("png")));
    EXPECT_EQ(host.written, "\x1b_Gi=2;EINVAL:invalid image dimensions\x1b\\");
    host.written.clear();
    host.width = 2;
    host.height = 2;
    host.bytes = 15;
    t.feed(apc("a=t,f=100,i=3", b64("png")));
    EXPECT_EQ(host.written, "\x1b_Gi=3;EINVAL:invalid decoded image extent\x1b\\");
    RecordingHost plain; // decodes no PNG at all
    Terminal u(20, 10, plain);
    u.feed(apc("a=t,f=100,i=4", b64("png")));
    EXPECT_EQ(plain.written, "\x1b_Gi=4;EBADPNG:failed to decode PNG data\x1b\\");
}

TEST(Graphics, DeleteModesSelectByPositionAndZ)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", b64(rgba(1, 1, 1, 1, 1))));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=2,q=2", b64(rgba(1, 1, 2, 2, 2))));
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
    t.t.feed(apc("a=t,f=32,s=1,v=1,I=7,q=2", b64(rgba(1, 1, 3, 3, 3))));
    t.t.feed(apc("a=p,I=7,C=1,q=2"));
    ASSERT_EQ(t.g().placements().size(), 2u);
    t.t.feed(apc("a=d,d=N,I=7"));
    EXPECT_EQ(left(), 1u);
    EXPECT_EQ(t.g().images().size(), 1u);
}

TEST(Graphics, UnsupportedDeleteKindsAreRefused)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", b64(rgba(1, 1, 1, 1, 1))));
    t.t.feed(apc("a=d,d=f,i=1"));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;EINVAL:unsupported delete target\x1b\\");
}

TEST(Graphics, ImageWhoseRowLeavesTheBufferOnReflowIsDropped)
{
    TerminalOptions o;
    o.maxScrollback = 0;
    Term t(10, 4, o);
    t.t.feed(apc("a=T,f=32,s=1,v=1,i=1,C=1,q=2", b64(rgba(1, 1, 0, 0, 0))));
    t.t.feed("\r\n0123456789\r\n0123456789\r\n0123456789");
    ASSERT_EQ(t.g().placements().size(), 1u);
    t.t.resize(3, 4, 10, 20); // the lines below need 12 rows: row 0 leaves
    EXPECT_TRUE(t.g().placements().empty());
    EXPECT_EQ(t.g().images().count(1), 1u);
}

TEST(Graphics, OmittedParentPlacementBindsAtCreation)
{
    Term t(40, 20);
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", b64(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=2,q=2", b64(rgba(1, 1, 9, 8, 7))));
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
    t.t.feed(apc("a=q,i=31,s=1,v=1,f=24", b64("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=31;OK\x1b\\");
    EXPECT_TRUE(t.g().images().empty());
    // A reset ends a transfer too.
    t.host.written.clear();
    t.t.feed(apc("a=t,i=2,f=24,s=1,v=1,m=1", "AA"));
    t.t.feed("\x1b" "c");
    t.t.feed(apc("a=t,i=3,f=24,s=1,v=1", b64("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=3;OK\x1b\\");
    EXPECT_EQ(t.g().images().count(2), 0u);
    // A transfer left unfinished on the other screen.
    t.host.written.clear();
    t.t.feed(apc("a=t,i=4,f=24,s=1,v=1,m=1", "AA"));
    t.t.feed("\x1b[?1049h");
    t.t.feed(apc("a=t,i=5,f=24,s=1,v=1", b64("abc")));
    t.t.feed("\x1b[?1049l");
    t.t.feed(apc("a=t,i=6,f=24,s=1,v=1", b64("abc")));
    EXPECT_EQ(t.host.written, "\x1b_Gi=5;OK\x1b\\\x1b_Gi=6;OK\x1b\\");
    EXPECT_EQ(t.g().images().count(4), 0u);
    EXPECT_EQ(t.g().images().count(6), 1u);
}

TEST(Graphics, AnonymousAndInternalImagesGoWithTheirLastPlacement)
{
    Term t(20, 10);
    for (int i = 0; i < 3; ++i) {
        t.t.feed(apc("a=T,f=24,s=1,v=1", b64("abc")));
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
        t.t.feed(apc("a=t,f=24,s=1,v=1,q=2,i=" + std::to_string(i), b64("abc")));
    }
    t.t.feed(apc("a=p,i=1,C=1,q=2"));
    t.host.written.clear();
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=5000", b64("abc")));
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
    t.t.feed(apc("a=t,i=1,f=24,s=1,v=1,q=2", b64("abc")));
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
    t.t.feed(apc("a=T,f=32,s=25,v=45,i=5,q=2", b64(rgba(25, 45, 0, 0, 255))));
    t.t.feed("\x1b[10;1H\n\n");
    auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, -20);
    t.t.feed("\n\n"); // now every row has left
    EXPECT_TRUE(t.g().visible(t.t.screen(), 10, 20).empty());
}

TEST(Graphics, QuotaEvictsOlderImagesNotTheOneBeingAdded)
{
    TerminalOptions o;
    o.graphicsQuota = 3000;
    Term t(20, 10, o);
    t.t.feed(apc("a=T,f=32,s=30,v=20,i=1,C=1,q=2", b64(rgba(30, 20, 1, 2, 3)))); // 2,400 bytes, shown
    t.host.written.clear();
    t.t.feed(apc("a=T,f=32,s=25,v=20,i=2,C=1", b64(rgba(25, 20, 4, 5, 6)))); // 2,000 bytes
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;OK\x1b\\");
    EXPECT_EQ(t.g().images().count(1), 0u);
    EXPECT_EQ(t.g().images().count(2), 1u);
    t.host.written.clear();
    t.t.feed(apc("a=t,f=32,s=30,v=30,i=3", b64(rgba(30, 30, 0, 0, 0)))); // 3,600 bytes: never fits
    EXPECT_EQ(t.host.written, "\x1b_Gi=3;ENOSPC:image larger than the storage quota\x1b\\");
    EXPECT_EQ(t.g().images().count(2), 1u);
}

TEST(Graphics, LowerQuotaEvictsAtOnce)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,f=32,s=30,v=20,i=1,C=1,q=2", b64(rgba(30, 20, 1, 2, 3))));
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
    s.apc("a=t,f=32,s=1,v=1,i=1,q=2", b64(rgba(1, 1, 0, 0, 0)));
    s.apc("a=p,U=1,i=1,c=2,r=1,q=2");
    s.screen.cursor.y = 2;
    s.apc("a=T,f=32,s=10,v=60,i=2,C=1,q=2", b64(rgba(10, 60, 0, 0, 0))); // rows 2-4
    s.screen.scrollUp(Region{0, 4, 0, 19}, 3, StyleTable::DefaultId, true); // its top row is history now
    s.apc("a=t,f=32,s=1,v=1,i=3,q=2", b64(rgba(1, 1, 0, 0, 0)));
    s.screen.cursor.y = 0;
    s.apc("a=p,i=3,C=1,q=2"); // wholly in the history after the next scroll
    s.screen.scrollUp(Region{0, 4, 0, 19}, 1, StyleTable::DefaultId, true);
    ASSERT_EQ(s.g.placements().size(), 3u);
    s.g.clearPlacements(s.screen, false, 10, 20);
    ASSERT_EQ(s.g.placements().size(), 2u);
    EXPECT_TRUE(std::any_of(s.g.placements().begin(), s.g.placements().end(), [](const Placement& p) { return p.isVirtual; }));
    EXPECT_TRUE(std::none_of(s.g.placements().begin(), s.g.placements().end(), [](const Placement& p) { return p.imageId == 2; }));
}

TEST(Graphics, EraseScrollbackKeepsImagesOnScreen)
{
    Store s(20, 5);
    s.apc("a=t,f=32,s=1,v=1,i=1,q=2", b64(rgba(1, 1, 0, 0, 0)));
    s.apc("a=p,i=1,p=1,C=1,q=2"); // row 0, scrolled into history below
    s.screen.scrollUp(Region{0, 4, 0, 19}, 3, StyleTable::DefaultId, true);
    s.apc("a=p,i=1,p=2,C=1,q=2"); // on the screen
    s.g.clearHistoryPlacements(s.screen);
    ASSERT_EQ(s.g.placements().size(), 1u);
    EXPECT_EQ(s.g.placements()[0].placementId, 2u);
}

TEST(Graphics, DeleteVisibleIncludesImagesReachingTheScreen)
{
    Term t(20, 10);
    t.t.feed(apc("a=T,f=32,s=10,v=200,i=1,q=2", b64(rgba(10, 200, 0, 0, 0)))); // ten rows from row 0
    t.t.feed("\n\n\n\n\n"); // its top five rows are history
    ASSERT_EQ(t.g().visible(t.t.screen(), 10, 20).size(), 1u);
    t.t.feed(apc("a=d,d=a"));
    EXPECT_TRUE(t.g().placements().empty());
}

TEST(Graphics, RangeDeleteReachesHighIds)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,q=2,i=3000000000", b64("abc")));
    ASSERT_EQ(t.g().images().count(3000000000u), 1u);
    t.t.feed(apc("a=d,d=R,x=1,y=4294967295"));
    EXPECT_EQ(t.g().images().count(3000000000u), 0u);
}

TEST(Graphics, RepliesToNumberedCommandsNameTheImage)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=24,s=1,v=1,I=13,q=2", b64("abc")));
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
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=7,q=2", b64(rgba(1, 1, 255, 0, 0))));
    t.t.feed(apc("a=f,f=32,s=1,v=1,i=7,z=100,q=2", b64(rgba(1, 1, 0, 255, 0))));
    if (placed) t.t.feed(apc("a=p,i=7,C=1,q=2"));
    t.t.feed(apc("a=a,i=7,s=3,v=1,q=2"));
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
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=1,q=2", b64("abc")));
    for (std::size_t i = 1; i < GraphicsStore::MaxFrames; ++i) {
        t.t.feed(apc("a=f,f=24,s=1,v=1,i=1,q=2", b64("abc")));
    }
    ASSERT_EQ(t.g().images().at(1).frames.size(), GraphicsStore::MaxFrames);
    t.host.written.clear();
    t.t.feed(apc("a=f,f=24,s=1,v=1,i=1", "AA")); // too short: decoding would say ENODATA
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;ENOSPC:too many image frames\x1b\\");
}

TEST(Graphics, SourceRectangleIsClippedToTheImage)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=4,v=4,i=1,q=2", b64(rgba(4, 4, 1, 2, 3))));
    t.t.feed(apc("a=p,i=1,p=1,x=10,c=2,r=2,C=1,q=2")); // starts past the edge: nothing to show
    t.t.feed(apc("a=p,i=1,p=2,x=2,w=10,C=1,q=2")); // wider than what is left
    const auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].srcX, 2);
    EXPECT_EQ(vis[0].srcW, 2);
    EXPECT_EQ(vis[0].width, 2);
}

TEST(Graphics, VirtualGridWithOneSideFollowsTheImageShape)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=200,v=200,i=1,q=2", b64(rgba(200, 200, 1, 2, 3))));
    t.t.feed(apc("a=p,U=1,i=1,c=2,q=2")); // 20 px wide, so 20 px high: one row
    EXPECT_TRUE(t.g().placeholderSlice(1, 0, 0, 0, 0, 0, 10, 20).has_value());
    EXPECT_FALSE(t.g().placeholderSlice(1, 0, 1, 0, 0, 0, 10, 20).has_value());
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
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=1,q=2", b64(rgba(1, 1, 1, 2, 3))));
    t.t.feed(apc("a=t,f=32,s=1,v=1,i=2,q=2", b64(rgba(1, 1, 3, 2, 1))));
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
    t.t.feed(apc("a=q,i=1,t=s,f=24,s=1,v=1", b64(shm.name)));
    EXPECT_NE(t.host.written.find("EBADF"), std::string::npos);
}

TEST(Graphics, FileTransmissionCanBeTurnedOff)
{
    TempFile file("afterglow-tty-graphics-protocol-");
    {
        const int fd = open(file.path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(write(fd, "RGBA", 4), 4);
        close(fd);
    }
    Store s(20, 5);
    s.g.setFileTransfer(false);
    s.apc("a=t,t=t,f=32,s=1,v=1,i=1", b64(file.path));
    EXPECT_EQ(s.host.written, "\x1b_Gi=1;EPERM:file transmission is turned off\x1b\\");
    EXPECT_TRUE(std::filesystem::exists(file.path)); // nothing opened or deleted
    s.host.written.clear();
    s.g.setFileTransfer(true);
    s.apc("a=t,t=t,f=32,s=1,v=1,i=1", b64(file.path));
    EXPECT_EQ(s.host.written, "\x1b_Gi=1;OK\x1b\\");
    EXPECT_FALSE(std::filesystem::exists(file.path)); // a temporary file is the terminal's to delete
}

TEST(Graphics, TerminalOptionTurnsFileTransmissionOff)
{
    TempFile file("afterglow-tty-graphics-protocol-option-");
    const auto writeFile = [&] {
        const int fd = open(file.path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(write(fd, "RGBA", 4), 4);
        close(fd);
    };
    writeFile();
    TerminalOptions off;
    off.graphicsFileTransfer = false;
    Term t(20, 10, off);
    t.t.feed(apc("a=t,t=t,f=32,s=1,v=1,i=1", b64(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;EPERM:file transmission is turned off\x1b\\");
    EXPECT_TRUE(std::filesystem::exists(file.path));
    // A reload that allows it again reaches the store.
    t.host.written.clear();
    t.t.setOptions(TerminalOptions{});
    t.t.feed(apc("a=t,t=t,f=32,s=1,v=1,i=1", b64(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=1;OK\x1b\\");
    // And one that turns it off again, on the alternate screen too.
    writeFile();
    t.host.written.clear();
    t.t.setOptions(off);
    t.t.feed("\x1b[?1049h");
    t.t.feed(apc("a=t,t=t,f=32,s=1,v=1,i=2", b64(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=2;EPERM:file transmission is turned off\x1b\\");
}

TEST(Graphics, RefusedCommandStillDeletesItsTemporaryFile)
{
    TempFile file("afterglow-tty-graphics-protocol-");
    {
        const int fd = open(file.path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(write(fd, "RGBA", 4), 4);
        close(fd);
    }
    Term t(20, 10);
    t.t.feed(apc("a=f,i=99,t=t,f=32,s=1,v=1", b64(file.path)));
    EXPECT_EQ(t.host.written, "\x1b_Gi=99;ENOENT:no such image\x1b\\");
    EXPECT_FALSE(std::filesystem::exists(file.path));
}

TEST(Graphics, PlacementLimitEvictsTheOldestInternalImage)
{
    Term t(20, 10);
    t.t.feed("\x1bPq#1~\x1b\\");
    ASSERT_EQ(t.g().placements().size(), 1u);
    const std::uint32_t first = t.g().placements()[0].imageId;
    t.t.feed(apc("a=t,f=24,s=1,v=1,i=1,q=2", b64("abc")));
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
    s.apc("a=t,f=32,s=1,v=1,i=1,q=2", b64(rgba(1, 1, 0, 0, 0)));
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

TEST(Base64, LenientPaddingEndsEachSegment)
{
    EXPECT_EQ(detail::base64Decode("QQ==Qg=="), "AB");
    EXPECT_EQ(detail::base64Decode("QUJD\nREVG"), "ABCDEF");
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
