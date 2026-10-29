// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

#include <gtest/gtest.h>

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
    Term(int cols, int rows)
        : t(cols, rows, host)
    {
        t.resize(cols + 1, rows, 10, 20); // give the terminal a cell size
        t.resize(cols, rows, 10, 20);
    }
    const GraphicsStore& g() const { return t.graphics(); }
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
    EXPECT_EQ(t.g().images().size(), 2u);
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
    t.t.feed("\x1b[2J");
    EXPECT_TRUE(t.g().placements().empty());
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

TEST(Graphics, FramesAndAnimation)
{
    Term t(20, 10);
    t.t.feed(apc("a=t,f=32,s=2,v=1,i=10,q=2", b64(rgba(2, 1, 255, 0, 0))));
    t.t.feed(apc("a=f,f=32,s=1,v=1,i=10,x=1,z=100,q=2", b64(rgba(1, 1, 0, 255, 0))));
    const Image& img = t.g().images().at(10);
    ASSERT_EQ(img.frames.size(), 2u);
    // new frame: transparent canvas with the patch at x=1
    EXPECT_EQ(img.frames[1]->rgba[3], 0);
    EXPECT_EQ(img.frames[1]->rgba[5], 255);
    EXPECT_EQ(img.frames[1]->gapMs, 100u);

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
    ASSERT_EQ(t.g().visible(t.t.screen(), 10, 20)[0].y, 40);
    t.t.resize(60, 30, 10, 20); // wider (reflow) and taller
    auto vis = t.g().visible(t.t.screen(), 10, 20);
    ASSERT_EQ(vis.size(), 1u);
    EXPECT_EQ(vis[0].y, 40);
    t.t.resize(30, 30, 10, 20);
    EXPECT_EQ(t.g().visible(t.t.screen(), 10, 20)[0].y, 40);
}

TEST(Graphics, ImageFollowsItsWrappedLine)
{
    Term t(10, 6);
    t.t.feed("0123456789AB"); // wraps onto row 1
    t.t.feed(apc("a=T,f=32,s=10,v=20,i=13,C=1,q=2", b64(rgba(10, 20, 0, 0, 0))));
    EXPECT_EQ(t.g().visible(t.t.screen(), 10, 20)[0].y, 20);
    t.t.resize(20, 6, 10, 20); // the line unwraps onto row 0
    EXPECT_EQ(t.g().visible(t.t.screen(), 10, 20)[0].y, 0);
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
