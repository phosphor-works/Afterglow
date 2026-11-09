// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The codec of the mux: a replica fed only by StateWriter frames must equal
// the terminal that parsed the bytes.

#include "TestUtil.h"

#include <AfterglowVt/StateSync.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <string>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {

TerminalOptions withScrollback(std::size_t rows)
{
    TerminalOptions o;
    o.maxScrollback = rows;
    return o;
}

/// The first difference of two screens, or an empty string.
std::string differs(const Screen& a, const Screen& b, const char* which)
{
    const auto where = [which](std::string what) { return std::string(which) + ": " + what; };
    if (a.cols() != b.cols() || a.rows() != b.rows()) return where("size");
    if (a.historySize() != b.historySize()) {
        return where("history " + std::to_string(a.historySize()) + " and " + std::to_string(b.historySize()));
    }
    if (a.rowsDropped() != b.rowsDropped()) return where("stable ids");
    if (a.cursor.x != b.cursor.x || a.cursor.y != b.cursor.y || a.cursor.pendingWrap != b.cursor.pendingWrap) {
        return where("cursor");
    }
    for (std::size_t i = 0; i < a.totalRows(); ++i) {
        const Row& ra = a.absoluteRow(i);
        const Row& rb = b.absoluteRow(i);
        const std::string at = "row " + std::to_string(i);
        if (ra.wrapped != rb.wrapped || ra.promptStart != rb.promptStart || ra.lineSize != rb.lineSize) return where(at + " flags");
        const std::size_t cells = std::max(ra.cells.size(), rb.cells.size());
        for (std::size_t x = 0; x < cells; ++x) {
            const Cell& ca = ra.at(int(x));
            const Cell& cb = rb.at(int(x));
            if (ca.codepoint() != cb.codepoint() || ca.width() != cb.width() || ca.hasGrapheme() != cb.hasGrapheme()
                || ca.isSpacerHead() != cb.isSpacerHead() || ca.isMulticell() != cb.isMulticell()
                || ca.multicellDx() != cb.multicellDx() || ca.multicellDy() != cb.multicellDy()) {
                return where(at + " cell " + std::to_string(x));
            }
            Style sa = a.styles().get(ca.styleId());
            Style sb = b.styles().get(cb.styleId());
            // Link indices differ between the two tables: their text counts.
            if (a.styles().linkUri(sa.link) != b.styles().linkUri(sb.link) || a.styles().linkId(sa.link) != b.styles().linkId(sb.link)) {
                return where(at + " link " + std::to_string(x) + " '" + std::string(a.styles().linkUri(sa.link)) + "' and '"
                             + std::string(b.styles().linkUri(sb.link)) + "' (indices " + std::to_string(sa.link) + " and "
                             + std::to_string(sb.link) + ", style ids " + std::to_string(ca.styleId()) + " and "
                             + std::to_string(cb.styleId()) + ")");
            }
            sa.link = sb.link = 0;
            if (!(sa == sb)) return where(at + " style " + std::to_string(x));
            if (a.graphemeExtra(ra, int(x)) != b.graphemeExtra(rb, int(x))) return where(at + " grapheme " + std::to_string(x));
            const MulticellParams* ma = a.multicellParams(ra, int(x));
            const MulticellParams* mb = b.multicellParams(rb, int(x));
            if (bool(ma) != bool(mb) || (ma && !(*ma == *mb))) return where(at + " multicell " + std::to_string(x));
        }
    }
    return {};
}

std::string differs(const Terminal& a, const Terminal& b)
{
    if (std::memcmp(&a.modes(), &b.modes(), sizeof(Modes)) != 0) return "modes";
    if (a.title() != b.title() || a.workingDirectory() != b.workingDirectory()) return "text";
    if (a.cursorShape() != b.cursorShape()) return "cursor shape";
    for (int i = 0; i < 256; ++i) {
        if (!(a.paletteColor(i) == b.paletteColor(i))) return "palette " + std::to_string(i);
    }
    if (!(a.defaultForeground() == b.defaultForeground()) || !(a.defaultBackground() == b.defaultBackground())) return "defaults";
    const auto same = [](const Rgb* x, const Rgb* y) { return bool(x) == bool(y) && (!x || *x == *y); };
    if (!same(a.cursorColor(), b.cursorColor()) || !same(a.cursorTextColor(), b.cursorTextColor())
        || !same(a.selectionForeground(), b.selectionForeground()) || !same(a.selectionBackground(), b.selectionBackground())
        || !same(a.visualBellColor(), b.visualBellColor())) {
        return "dynamic colors";
    }
    if (std::string d = differs(a.primaryScreen(), b.primaryScreen(), "primary"); !d.empty()) return d;
    if (a.modes().altScreen) return differs(a.screen(), b.screen(), "alternate");
    return {};
}

/// A terminal that parses, and a replica that only takes frames.
struct Pair
{
    RecordingHost sourceHost;
    RecordingHost replicaHost;
    Terminal source;
    Terminal replica;
    StateWriter writer;
    StateReader reader;
    std::size_t budget;
    int frames = 0;

    Pair(int cols, int rows, std::size_t scrollback, std::size_t budget = 1 << 20, std::size_t replicaScrollback = 0)
        : source(cols, rows, sourceHost, withScrollback(scrollback))
        // Another size: the first frame gives the replica the source's.
        , replica(20, 5, replicaHost, withScrollback(replicaScrollback ? replicaScrollback : scrollback))
        , budget(budget)
    {
    }

    /// Frames until the writer has nothing left. False when the reader
    /// refused one: @p error says why.
    bool sync(std::string* error = nullptr)
    {
        for (int guard = 0; guard < 100000; ++guard) {
            const std::string frame = writer.update(source, budget);
            if (frame.empty() && !writer.pending()) return true;
            ++frames;
            std::string why;
            if (!reader.apply(replica, frame, &why)) {
                if (error) *error = why;
                return false;
            }
        }
        if (error) *error = "the writer never caught up";
        return false;
    }
};

} // namespace

TEST(StateSync, TextStylesLinksAndText)
{
    Pair p(30, 6, 100);
    p.source.feed("plain \x1b[1;31mbold red\x1b[0m \x1b[38;2;10;20;30;48;5;200mrgb\x1b[0m\r\n");
    p.source.feed("wide \xe4\xb8\xad\xe6\x96\x87 and \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9 family\r\n");
    p.source.feed("\x1b]8;id=x;https://example.com\x1b\\link\x1b]8;;\x1b\\ after\r\n");
    p.source.feed("\x1b[4:3;58:2::255:0:0mcurly\x1b[0m\x1b]2;the title\x1b\\\x1b]7;file:///tmp\x1b\\");
    p.source.feed("\x1b[5 q\x1b]4;3;rgb:12/34/56\x1b\\\x1b]12;rgb:aa/bb/cc\x1b\\");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
    EXPECT_EQ(p.replica.title(), "the title");
}

TEST(StateSync, NothingChangedIsNoFrame)
{
    Pair p(20, 4, 10);
    p.source.feed("x");
    ASSERT_TRUE(p.sync());
    EXPECT_EQ(p.writer.update(p.source, p.budget), "");
    // One changed cell sends its row, not the screen.
    p.source.feed("y");
    const std::string frame = p.writer.update(p.source, p.budget);
    EXPECT_FALSE(frame.empty());
    EXPECT_LT(frame.size(), 200u);
    ASSERT_TRUE(p.reader.apply(p.replica, frame, nullptr));
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, ScrollingFillsTheHistoryInSmallFrames)
{
    Pair p(40, 10, 500, 2048);
    for (int i = 0; i < 300; ++i) {
        p.source.feed("line " + std::to_string(i) + " \x1b[3" + std::to_string(i % 8) + "mcolor\x1b[0m\r\n");
        if (i % 37 == 0) {
            ASSERT_TRUE(p.sync());
        }
    }
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
    EXPECT_GT(p.frames, 10);
}

TEST(StateSync, ALateReplicaGetsTheScreenFirstThenTheHistory)
{
    Pair p(40, 8, 1000, 4096);
    for (int i = 0; i < 600; ++i) p.source.feed("history line " + std::to_string(i) + "\r\n");
    p.source.feed("on screen");
    // The first frame shows the screen, before the history is complete.
    const std::string first = p.writer.update(p.source, p.budget);
    ASSERT_TRUE(p.reader.apply(p.replica, first, nullptr));
    EXPECT_TRUE(p.writer.pending());
    EXPECT_LT(p.replica.primaryScreen().historySize(), p.source.primaryScreen().historySize());
    EXPECT_EQ(rowText(p.replica, 7), "on screen");
    // Output that arrives during the backfill lands in order too.
    p.source.feed("\r\nmore\r\nand more\r\n");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, AScreenLargerThanTheBudgetGoesInSeveralFrames)
{
    // Each cell carries combining marks, so the visible rows alone are far
    // past the budget: no frame may be much larger than it.
    Pair p(60, 20, 0, 4096);
    std::string row;
    for (int x = 0; x < 59; ++x) row += "a\xcc\x81\xcc\x82\xcc\x83";
    for (int y = 0; y < 20; ++y) p.source.feed(row + (y < 19 ? "\r\n" : ""));
    std::size_t largest = 0;
    int frames = 0;
    for (int guard = 0; guard < 1000; ++guard) {
        const std::string frame = p.writer.update(p.source, p.budget);
        if (frame.empty() && !p.writer.pending()) break;
        ++frames;
        largest = std::max(largest, frame.size());
        std::string error;
        ASSERT_TRUE(p.reader.apply(p.replica, frame, &error)) << error;
    }
    EXPECT_GT(frames, 1);
    // The budget and one row, which always goes.
    EXPECT_LT(largest, p.budget + row.size() + 512);
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, EachSynchronizedUpdateReachesTheReplica)
{
    // An update that ends and the next that begins between two frames: the
    // replica sees the mode on in both, and still learns that a new update
    // began, so that its renderer gives it a timeout of its own.
    Pair p(20, 4, 0);
    p.source.feed("\x1b[?2026h");
    ASSERT_TRUE(p.sync());
    const std::uint64_t first = p.replica.synchronizedOutputSerial();
    EXPECT_TRUE(p.replica.modes().synchronizedOutput);
    p.source.feed("x\x1b[?2026l\x1b[?2026h");
    ASSERT_TRUE(p.sync());
    EXPECT_TRUE(p.replica.modes().synchronizedOutput);
    EXPECT_EQ(p.replica.synchronizedOutputSerial(), p.source.synchronizedOutputSerial());
    EXPECT_NE(p.replica.synchronizedOutputSerial(), first);
}

TEST(StateSync, AlternateScreenAndBack)
{
    Pair p(30, 6, 50);
    p.source.feed("primary text\r\n");
    ASSERT_TRUE(p.sync());
    p.source.feed("\x1b[?1049h\x1b[2J\x1b[Hfull screen app\x1b[3;5Hthere");
    ASSERT_TRUE(p.sync());
    EXPECT_TRUE(p.replica.modes().altScreen);
    EXPECT_EQ(differs(p.source, p.replica), "");
    p.source.feed("\x1b[?1049l");
    ASSERT_TRUE(p.sync());
    EXPECT_FALSE(p.replica.modes().altScreen);
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, AResizeStartsTheScreenOver)
{
    Pair p(40, 8, 200);
    for (int i = 0; i < 60; ++i) p.source.feed("a fairly long line number " + std::to_string(i) + "\r\n");
    ASSERT_TRUE(p.sync());
    p.source.resize(25, 8, 0, 0);
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
    p.source.resize(25, 12, 0, 0);
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, ClearedAndDroppedHistory)
{
    Pair p(30, 5, 40, 1024);
    for (int i = 0; i < 100; ++i) p.source.feed("x" + std::to_string(i) + "\r\n");
    ASSERT_TRUE(p.sync());
    // Many more rows than the history holds, between two frames: the rows
    // that scrolled out before a frame could carry them leave a gap.
    for (int i = 0; i < 400; ++i) p.source.feed("y" + std::to_string(i) + "\r\n");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
    p.source.feed("\x1b[3J");
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(p.replica.primaryScreen().historySize(), 0u);
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, AReplicaWithLessHistoryKeepsTheNewestRows)
{
    // A window whose Terminal.scrollback is lower than the daemon's.
    Pair p(30, 4, 300, 2048, 50);
    for (int i = 0; i < 280; ++i) p.source.feed("row " + std::to_string(i) + "\r\n");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    const Screen& replica = p.replica.primaryScreen();
    ASSERT_EQ(replica.historySize(), 50u);
    EXPECT_EQ(rowText(replica, replica.absoluteRow(0)), "row 227");
    EXPECT_EQ(rowText(replica, replica.absoluteRow(49)), "row 276");
    EXPECT_EQ(differs(p.source.screen(), p.replica.screen(), "x").find("history"), 3u);
}

TEST(StateSync, RandomOutputStaysEqual)
{
    // Text, styles, links, wide and combining characters, scrolling, margins,
    // erasures, insertions, the alternate screen, modes and colors, at random.
    const std::vector<std::string> pieces = {
        "abc", "hello world ", "\r\n", "\n", "\r", "\t", "\b", "\xe4\xb8\xad", "e\xcc\x81", "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd",
        "\x1b[1m", "\x1b[0m", "\x1b[3%dm", "\x1b[4%dm", "\x1b[38;5;%dm", "\x1b[48;2;%d;40;50m", "\x1b[7m", "\x1b[4:2m",
        "\x1b[%d;%dH", "\x1b[%dA", "\x1b[%dC", "\x1b[K", "\x1b[1K", "\x1b[2J", "\x1b[J", "\x1b[%dL", "\x1b[%dM", "\x1b[%d@",
        "\x1b[%dP", "\x1b[%dX", "\x1b[%dS", "\x1b[%dT", "\x1b[2;%dr", "\x1b[r", "\x1b\x4d", "\x1b\x44", "\x1b\x37", "\x1b\x38",
        "\x1b[?1049h", "\x1b[?1049l", "\x1b[?47h", "\x1b[?47l", "\x1b[?25l", "\x1b[?25h", "\x1b[?1000h", "\x1b[?1006h",
        "\x1b[?2004h", "\x1b[?7l", "\x1b[?7h", "\x1b]8;;https://x/%d\x1b\\", "\x1b]8;;\x1b\\", "\x1b]4;%d;rgb:10/20/30\x1b\\",
        "\x1b]2;title %d\x1b\\", "\x1b#6", "\x1b#3", "\x1b]66;s=2;ab\x1b\\", "\x1b[3J", "\x1b[%d q", "\x1b\x63",
    };
    for (unsigned seed = 1; seed <= 60; ++seed) {
        std::mt19937 rng(seed);
        Pair p(20 + int(rng() % 40), 4 + int(rng() % 12), rng() % 3 ? 60 : 0, 256 + rng() % 4096);
        for (int step = 0; step < 1500; ++step) {
            std::string piece = pieces[rng() % pieces.size()];
            for (std::size_t at; (at = piece.find("%d")) != std::string::npos;) piece.replace(at, 2, std::to_string(rng() % 20));
            p.source.feed(piece);
            if (rng() % 9 == 0) {
                std::string error;
                ASSERT_TRUE(p.sync(&error)) << "seed " << seed << " step " << step << ": " << error;
                const std::string d = differs(p.source, p.replica);
                ASSERT_TRUE(d.empty()) << "seed " << seed << " step " << step << ": " << d;
            }
            if (step == 700) p.source.resize(15 + int(rng() % 40), 3 + int(rng() % 10), 0, 0);
        }
    }
}

TEST(StateSync, ReaderRefusesBrokenFrames)
{
    Pair p(20, 4, 10);
    p.source.feed("\x1b[31mred\x1b[0m\r\nmore");
    const std::string frame = p.writer.update(p.source, p.budget);
    ASSERT_FALSE(frame.empty());
    std::string error;
    {
        StateReader reader;
        std::string bad = frame;
        bad[0] = char(99);
        EXPECT_FALSE(reader.apply(p.replica, bad, &error));
        EXPECT_NE(error.find("99"), std::string::npos) << error;
    }
    {
        StateReader reader;
        EXPECT_FALSE(reader.apply(p.replica, frame.substr(0, frame.size() / 2), &error));
    }
    {
        StateReader reader;
        EXPECT_FALSE(reader.apply(p.replica, std::string("\x01\x63\x00", 3), &error));
        EXPECT_NE(error.find("unknown"), std::string::npos) << error;
    }
    // Random damage: refused or applied, never a crash or a leak (the
    // sanitizer build runs this too).
    std::mt19937 rng(7);
    for (int i = 0; i < 2000; ++i) {
        std::string bad = frame;
        for (int k = 0; k < 1 + int(rng() % 4); ++k) bad[1 + rng() % (bad.size() - 1)] = char(rng());
        RecordingHost host;
        Terminal replica(20, 4, host, withScrollback(10));
        StateReader reader;
        reader.apply(replica, bad, &error);
        // The same reader takes the first frame of a writer that starts
        // over, which gives the replica everything again.
        StateWriter writer;
        EXPECT_TRUE(reader.apply(replica, writer.update(p.source, p.budget), &error)) << error;
        EXPECT_EQ(differs(p.source, replica), "");
    }
}

TEST(StateSync, ReaderRefusesALinkWithoutAUri)
{
    RecordingHost host;
    Terminal replica(20, 4, host);
    StateReader reader;
    // The version, then a links section of the primary screen: one link,
    // number 1, with an empty URI and an empty id.
    const std::string frame("\x01\x06\x05\x00\x01\x01\x00\x00", 8);
    std::string error;
    EXPECT_FALSE(reader.apply(replica, frame, &error));
    EXPECT_NE(error.find("link"), std::string::npos) << error;
}

TEST(StateSync, ASizedBlockKeepsItsLongText)
{
    // OSC 66 with w= puts all of its text in one block: more codepoints than
    // one cell keeps.
    Pair p(40, 4, 20, 1024);
    p.source.feed("\x1b]66;w=2;" + std::string(300, 'x') + "\x1b\\\r\n");
    const Screen& source = p.source.screen();
    ASSERT_GT(source.graphemeExtra(source.row(0), 0).size(), 64u);
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
    // In the history, and for a replica that starts late.
    p.source.feed("\n\n\n\n\n");
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
    p.writer.reset();
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, AFlattenedBlockKeepsItsTextOnANarrowerScreen)
{
    // A width change turns a block into one plain cell with all of its text:
    // more than the new width gives a block.
    Pair p(200, 4, 20);
    p.source.feed("\x1b]66;w=2;" + std::string(3000, 'x') + "\x1b\\");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    p.source.resize(40, 4, 0, 0);
    const Screen& source = p.source.screen();
    ASSERT_GT(source.graphemeExtra(source.row(0), 0).size(), 40u * Screen::MaxGraphemeExtra);
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, AHistoryRowLargerThanTheBudgetStillGoes)
{
    // A late replica gets the history newest first. Each of these rows
    // takes more bytes than a frame may have, so one row goes in each frame.
    Pair p(200, 4, 30, 512);
    for (int i = 0; i < 20; ++i) {
        std::string line;
        for (int x = 0; x < 200; ++x) line += "\x1b[3" + std::to_string(x % 8) + "m" + char('a' + x % 26);
        p.source.feed(line + "\x1b[0m\r\n");
    }
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, TheDefinitionsOfHistoryRowsCountInTheBudget)
{
    // Each history row has a link of its own, with a long URI.
    Pair p(30, 4, 100, 4096);
    for (int i = 0; i < 40; ++i) {
        p.source.feed("\x1b]8;;https://example.com/" + std::string(1000, char('a' + i % 26)) + std::to_string(i)
                      + "\x1b\\link\x1b]8;;\x1b\\\r\n");
    }
    // The first frame has the visible rows, which always go. After it, a
    // frame passes the budget by one row and its link at most.
    std::size_t largest = 0;
    bool first = true;
    for (int guard = 0; guard < 1000; ++guard) {
        const std::string frame = p.writer.update(p.source, p.budget);
        if (frame.empty() && !p.writer.pending()) break;
        if (!first) largest = std::max(largest, frame.size());
        first = false;
        std::string error;
        ASSERT_TRUE(p.reader.apply(p.replica, frame, &error)) << error;
    }
    EXPECT_FALSE(p.writer.pending());
    EXPECT_EQ(differs(p.source, p.replica), "");
    EXPECT_LE(largest, p.budget + 1100);
}

TEST(StateSync, AScrolledBackViewStaysOnItsRows)
{
    Pair p(30, 5, 200);
    for (int i = 0; i < 50; ++i) p.source.feed("line " + std::to_string(i) + "\r\n");
    ASSERT_TRUE(p.sync());
    p.source.scrollViewport(10);
    p.replica.scrollViewport(10);
    const Screen& replica = p.replica.screen();
    const std::string top = rowText(replica, replica.absoluteRow(replica.viewportTop()));
    for (int i = 0; i < 3; ++i) p.source.feed("more " + std::to_string(i) + "\r\n");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(replica.viewportOffset(), p.source.screen().viewportOffset());
    EXPECT_EQ(rowText(replica, replica.absoluteRow(replica.viewportTop())), top);
    // More rows than the history holds: the view ends on the oldest row, as
    // it does in the terminal that parsed them.
    for (int i = 0; i < 300; ++i) p.source.feed("flood\r\n");
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(replica.viewportOffset(), p.source.screen().viewportOffset());
    EXPECT_EQ(differs(p.source, p.replica), "");
}
