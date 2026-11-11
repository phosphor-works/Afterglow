// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The codec of the mux: a replica fed only by StateWriter frames must equal
// the terminal that parsed the bytes.

#include "TestUtil.h"

#include <AfterglowVt/StateSync.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <map>
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

/// The first difference of the OSC 133 regions as each terminal reports them.
std::string regionsDiffer(const Terminal& a, const Terminal& b)
{
    const auto ra = a.semanticRegions();
    const auto rb = b.semanticRegions();
    if (ra.size() != rb.size()) return "count " + std::to_string(ra.size()) + " and " + std::to_string(rb.size());
    for (std::size_t i = 0; i < ra.size(); ++i) {
        const SemanticRegion& x = ra[i];
        const SemanticRegion& y = rb[i];
        if (x.id != y.id || x.kind != y.kind || x.start.row != y.start.row || x.start.col != y.start.col
            || x.end.row != y.end.row || x.end.col != y.end.col || x.open != y.open || x.exitCode != y.exitCode
            || x.available != y.available || x.startTrimmed != y.startTrimmed) {
            return "region " + std::to_string(i);
        }
        if (a.semanticRegionText(x.id) != b.semanticRegionText(y.id)) return "the text of region " + std::to_string(i);
    }
    return {};
}

/// The first difference of the program status records, or an empty string.
std::string statusDiffers(const Terminal& a, const Terminal& b)
{
    const auto& ra = a.programStatus().records();
    const auto& rb = b.programStatus().records();
    if (ra.size() != rb.size()) return "status count";
    for (std::size_t i = 0; i < ra.size(); ++i) {
        const StatusRecord& x = ra[i];
        const StatusRecord& y = rb[i];
        if (x.id != y.id || x.state != y.state || x.kind != y.kind || x.progress != y.progress || x.app != y.app
            || x.title != y.title || x.msg != y.msg || x.serial != y.serial) {
            return "status record " + std::to_string(i);
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
    if (a.modes().altScreen) {
        if (std::string d = differs(a.screen(), b.screen(), "alternate"); !d.empty()) return d;
    }
    // What a frame carries besides the screens.
    if (std::string d = regionsDiffer(a, b); !d.empty()) return "shell marks: " + d;
    if (a.atShellPrompt() != b.atShellPrompt() || a.promptTakesClicks() != b.promptTakesClicks()
        || a.hasCommandOutput() != b.hasCommandOutput() || a.promptRedrawPending() != b.promptRedrawPending()) {
        return "shell state";
    }
    if (std::string d = statusDiffers(a, b); !d.empty()) return d;
    if (!(a.contexts().stack() == b.contexts().stack())) return "contexts";
    if (a.extraCursors() != b.extraCursors() || !(a.extraCursorColor() == b.extraCursorColor())
        || !(a.extraCursorTextColor() == b.extraCursorTextColor())) {
        return "extra cursors";
    }
    return {};
}

/// @p v as a varint of the codec.
std::string varint(std::uint64_t v)
{
    std::string out;
    while (v >= 0x80) {
        out.push_back(char((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(char(v));
    return out;
}

/// A host that also hears of program status and region scrolls.
struct SyncHost : RecordingHost
{
    const Terminal* terminal = nullptr;
    /// The source's: where its scrolls wait for the next frame, as the
    /// daemon's pane keeps them.
    RegionScrolls* record = nullptr;
    /// What the host heard, with the screen that was active then.
    std::vector<std::pair<bool, RegionScroll>> scrolls;
    int statusChanges = 0;
    void programStatusChanged() override { ++statusChanges; }
    void regionScrolled(const RegionScroll& s) override
    {
        if (record) record->add(terminal->modes().altScreen, s);
        scrolls.emplace_back(terminal->modes().altScreen, s);
        // The screen that the host reads is the one that scrolled.
        activeMatches = activeMatches && (&terminal->screen() == &terminal->primaryScreen()) == !terminal->modes().altScreen;
    }
    bool activeMatches = true;
};

/// A terminal that parses, and a replica that only takes frames.
struct Pair
{
    SyncHost sourceHost;
    SyncHost replicaHost;
    Terminal source;
    Terminal replica;
    RegionScrolls scrolls;
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
        sourceHost.terminal = &source;
        sourceHost.record = &scrolls;
        replicaHost.terminal = &replica;
    }

    /// The writer's next frame, with the scrolls since the last one, as the
    /// daemon's pane makes it.
    std::string update()
    {
        std::string frame = writer.update(source, budget, &scrolls);
        scrolls.clear();
        return frame;
    }

    /// Frames until the writer has nothing left. False when the reader
    /// refused one: @p error says why.
    bool sync(std::string* error = nullptr)
    {
        for (int guard = 0; guard < 100000; ++guard) {
            const std::string frame = update();
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

TEST(StateSync, ShellIntegrationMarksAndRegions)
{
    Pair p(30, 8, 100);
    p.source.feed("\x1b]133;A;click_events=1\x1b\\$ \x1b]133;B\x1b\\ls\r\n\x1b]133;C\x1b\\one\r\ntwo\r\n\x1b]133;D;3\x1b\\");
    p.source.feed("\x1b]133;A;click_events=1\x1b\\$ \x1b]133;B\x1b\\");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
    EXPECT_EQ(regionsDiffer(p.source, p.replica), "");
    ASSERT_FALSE(p.source.semanticRegions().empty());
    // The last output, and a click at the prompt that moves the cursor.
    EXPECT_TRUE(p.replica.hasCommandOutput());
    EXPECT_EQ(p.replica.text(Terminal::TextScope::LastOutput), p.source.text(Terminal::TextScope::LastOutput));
    EXPECT_NE(p.replica.text(Terminal::TextScope::LastOutput), "");
    EXPECT_TRUE(p.replica.atShellPrompt());
    EXPECT_TRUE(p.replica.promptTakesClicks());
    // A command runs: the replica leaves the prompt with the source.
    p.source.feed("sleep 1\r\n\x1b]133;C\x1b\\");
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_FALSE(p.replica.atShellPrompt());
    EXPECT_FALSE(p.replica.promptTakesClicks());
    EXPECT_EQ(regionsDiffer(p.source, p.replica), "");
    // Output past the screen: the regions keep their rows by stable id.
    for (int i = 0; i < 20; ++i) p.source.feed("line\r\n");
    p.source.feed("\x1b]133;D;0\x1b\\");
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(regionsDiffer(p.source, p.replica), "");
    EXPECT_EQ(p.replica.text(Terminal::TextScope::LastOutput), p.source.text(Terminal::TextScope::LastOutput));
}

TEST(StateSync, ProgramStatusAndContextsReachTheReplicasHost)
{
    Pair p(30, 6, 100);
    ASSERT_TRUE(p.sync());
    const int statusChanges = p.replicaHost.statusChanges;
    const int contextChanges = p.replicaHost.contextChanges;
    p.source.feed("\x1b]7501;state=blocked:kind=permission:app=terraform:msg=QXBwbHkgMz8\x1b\\");
    p.source.feed("\x1b]7501;id=sub:state=working:progress=40\x1b\\");
    p.source.feed("\x1b]3008;start=a1;type=shell;user=someone;hostname=box\x1b\\");
    p.source.feed("\x1b]3008;start=b2;type=elevate;user=someone;targetuser=root;hostname=box\x1b\\");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    const auto& a = p.source.programStatus();
    const auto& b = p.replica.programStatus();
    ASSERT_EQ(a.records().size(), 2u);
    ASSERT_EQ(b.records().size(), a.records().size());
    for (std::size_t i = 0; i < a.records().size(); ++i) {
        const StatusRecord& x = a.records()[i];
        const StatusRecord& y = b.records()[i];
        EXPECT_EQ(x.id, y.id);
        EXPECT_EQ(x.state, y.state);
        EXPECT_EQ(x.kind, y.kind);
        EXPECT_EQ(x.progress, y.progress);
        EXPECT_EQ(x.app, y.app);
        EXPECT_EQ(x.title, y.title);
        EXPECT_EQ(x.msg, y.msg);
        EXPECT_EQ(x.serial, y.serial);
        EXPECT_EQ(a.effectiveApp(x), b.effectiveApp(y));
    }
    EXPECT_TRUE(p.source.contexts().stack() == p.replica.contexts().stack());
    ASSERT_EQ(p.replica.contexts().stack().size(), 2u);
    // One call for each frame that changed them, as a terminal makes for
    // each report that changes them.
    EXPECT_EQ(p.replicaHost.statusChanges, statusChanges + 1);
    EXPECT_EQ(p.replicaHost.contextChanges, contextChanges + 1);
    // A frame that changes neither calls nothing.
    p.source.feed("x");
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(p.replicaHost.statusChanges, statusChanges + 1);
    EXPECT_EQ(p.replicaHost.contextChanges, contextChanges + 1);
    // A new prompt ends what was in flight, and the outer context ends both.
    p.source.feed("\x1b]133;A\x1b\\\x1b]3008;end=a1\x1b\\");
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(b.records().size(), a.records().size());
    EXPECT_TRUE(p.replica.contexts().stack().empty());
    EXPECT_EQ(p.replicaHost.statusChanges, statusChanges + 2);
    EXPECT_EQ(p.replicaHost.contextChanges, contextChanges + 2);
}

TEST(StateSync, ExtraCursorsAndTheirColors)
{
    Pair p(30, 6, 100);
    p.source.feed("\x1b[>2;4:2:3:3:5 q\x1b[>29;2:5:1 q\x1b[>40;2:255:0:0 q\x1b[>30;5:12 q");
    ASSERT_EQ(p.source.extraCursors().size(), 7u);
    ASSERT_EQ(p.source.extraCursorColor().mode, ExtraCursorColor::Mode::Rgb);
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_TRUE(p.replica.extraCursors() == p.source.extraCursors());
    EXPECT_TRUE(p.replica.extraCursorColor() == p.source.extraCursorColor());
    EXPECT_TRUE(p.replica.extraCursorTextColor() == p.source.extraCursorTextColor());
    p.source.feed("\x1b[>0;4 q");
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_TRUE(p.replica.extraCursors().empty());
}

TEST(StateSync, TheReplicasHostHearsOfRegionScrolls)
{
    Pair p(20, 6, 100);
    p.source.feed("\n\n\n\n\n\n\n\n");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    p.replicaHost.scrolls.clear();
    p.sourceHost.scrolls.clear();
    // Two scrolls of one region in one direction, then the other way, then
    // another region: three scrolls for the replica's host.
    p.source.feed("\x1b[2;4r\x1b[S\x1b[2S\x1b[T\x1b[3;1H\x1b[2L");
    ASSERT_EQ(p.sourceHost.scrolls.size(), 4u);
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
    ASSERT_EQ(p.replicaHost.scrolls.size(), 3u);
    EXPECT_EQ(p.replicaHost.scrolls[0].second.delta, -3);
    EXPECT_EQ(p.replicaHost.scrolls[0].second.top, p.sourceHost.scrolls[0].second.top);
    EXPECT_EQ(p.replicaHost.scrolls[0].second.bottom, p.sourceHost.scrolls[0].second.bottom);
    EXPECT_EQ(p.replicaHost.scrolls[1].second.delta, 1);
    EXPECT_EQ(p.replicaHost.scrolls[2].second.delta, p.sourceHost.scrolls[3].second.delta);
    EXPECT_EQ(p.replicaHost.scrolls[2].second.top, p.sourceHost.scrolls[3].second.top);
    for (const auto& [alt, scroll] : p.replicaHost.scrolls) EXPECT_FALSE(alt);
    // On the alternate screen, which the frame then leaves: the host hears
    // of the scroll with the screen that scrolled.
    p.replicaHost.scrolls.clear();
    p.source.feed("\x1b[?1049h\x1b[2;4r\x1b[S\x1b[?1049l");
    ASSERT_TRUE(p.sync(&error)) << error;
    ASSERT_EQ(p.replicaHost.scrolls.size(), 1u);
    EXPECT_TRUE(p.replicaHost.scrolls[0].first);
    EXPECT_FALSE(p.replica.modes().altScreen);
    EXPECT_TRUE(p.replicaHost.activeMatches);
    // A new replica keeps nothing that the scrolls move: its first frame
    // leaves them out.
    p.source.feed("\x1b[2;4r\x1b[S");
    p.writer.reset();
    p.replicaHost.scrolls.clear();
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_TRUE(p.replicaHost.scrolls.empty());
}

TEST(StateSync, ExactlyTheMostRegionScrollsAllGo)
{
    Pair p(20, 6, 100);
    p.source.feed("\n\n\n\n\n\n\n\n");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    p.replicaHost.scrolls.clear();
    // Two regions in turn: no scroll continues the one before it.
    for (std::size_t i = 0; i < RegionScrolls::Max; ++i) p.source.feed(i % 2 ? "\x1b[2;4r\x1b[S" : "\x1b[3;5r\x1b[S");
    ASSERT_TRUE(p.sync(&error)) << error;
    ASSERT_EQ(p.replicaHost.scrolls.size(), RegionScrolls::Max);
    for (const auto& [alt, scroll] : p.replicaHost.scrolls) EXPECT_EQ(scroll.delta, -1);
}

TEST(StateSync, TooManyRegionScrollsMoveTheRowsThatTheyTouchedOut)
{
    Pair p(20, 6, 100);
    p.source.feed("\n\n\n\n\n\n\n\n");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    p.replicaHost.scrolls.clear();
    const Screen& primary = p.replica.primaryScreen();
    const std::uint64_t firstVisible = primary.rowsDropped() + primary.historySize();
    // One past the most: two regions in turn, rows 2 to 5 of the screen.
    for (std::size_t i = 0; i <= RegionScrolls::Max; ++i) p.source.feed(i % 2 ? "\x1b[2;4r\x1b[S" : "\x1b[3;5r\x1b[S");
    ASSERT_TRUE(p.sync(&error)) << error;
    // Only the primary screen scrolled: one scroll from the top of the higher
    // region to the bottom of the lower, which every row in it leaves.
    ASSERT_EQ(p.replicaHost.scrolls.size(), 1u);
    const auto& [alt, scroll] = p.replicaHost.scrolls[0];
    EXPECT_FALSE(alt);
    EXPECT_TRUE(scroll.fullWidth);
    EXPECT_EQ(scroll.top, firstVisible + 1);
    EXPECT_EQ(scroll.bottom, firstVisible + 4);
    EXPECT_EQ(scroll.delta, -4);
    for (std::uint64_t row = scroll.top; row <= scroll.bottom; ++row) {
        const SelectionPoint at{row, 3};
        EXPECT_TRUE(scroll.contains(at));
        EXPECT_FALSE(scroll.map(at).has_value());
    }
    // The history and the rows outside the regions stay where they are.
    EXPECT_FALSE(scroll.contains(SelectionPoint{primary.rowsDropped(), 3}));
    EXPECT_FALSE(scroll.contains(SelectionPoint{firstVisible, 3}));
}

TEST(StateSync, ReaderRefusesBadNewSections)
{
    Pair p(20, 4, 10);
    ASSERT_TRUE(p.sync());
    // A version byte, then one section: a tag, its length, its payload.
    const auto frame = [](std::uint8_t tag, std::string payload) {
        std::string out(1, char(1));
        out.push_back(char(tag));
        out.push_back(char(payload.size()));
        return out + payload;
    };
    StateReader reader;
    for (const auto& [name, bytes] : std::vector<std::pair<const char*, std::string>>{
             {"a scroll of no rows", frame(15, std::string("\x00\x01\x00\x05\x02\x00\x01\x01\x01", 9))},
             {"a scroll that moves nothing", frame(15, std::string("\x00\x01\x00\x01\x02\x00\x01\x01\x00", 9))},
             {"a shell zone past Input", frame(11, std::string("\x03\x00\x00\x00\x00\x00\x00\x00\x00", 9))},
             {"a region id past the next one", frame(11, std::string("\x00\x00\x00\x00\x00\x00\x00\x01\x01\x01\x00\x00\x00\x00\x00", 15))},
             {"a prompt redraw of 2", frame(11, std::string("\x00\x00\x00\x02\x00\x00\x00\x00\x00", 9))},
             {"a shell mark that is valid twice", frame(11, std::string("\x00\x00\x00\x00\x02", 5))},
             {"a region of kind 3", frame(11, std::string("\x00\x00\x00\x00\x00\x00\x00\x02\x01\x00\x03\x00\x00\x00\x00", 15))},
             {"a region with an unknown flag", frame(11, std::string("\x00\x00\x00\x00\x00\x00\x00\x02\x01\x00\x10\x00\x00\x00\x00", 15))},
             {"a lost flag of 2", frame(15, std::string("\x02\x00", 2))},
             {"a varint of more than 64 bits", frame(15, std::string("\x00\x80\x80\x80\x80\x80\x80\x80\x80\x80\x02", 11))},
             {"a status kind past Auth", frame(12, std::string("\x01\x00\x00\x7f\x01\x00\x00\x00\x01", 9))},
             {"status records out of order", frame(12, std::string("\x02\x01\x62\x00\x00\x01\x00\x00\x00\x01\x01\x61\x00\x00\x01\x00\x00\x00\x01", 19))},
             {"a status record twice", frame(12, std::string("\x02\x01\x61\x00\x00\x01\x00\x00\x00\x01\x01\x61\x00\x00\x01\x00\x00\x00\x01", 19))},
             {"a status id with a control character", frame(12, std::string("\x01\x01\x07\x00\x00\x01\x00\x00\x00\x01", 10))},
             {"a context remote byte of 2", frame(13, std::string("\x01\x01\x61\x02\x00\x00\x00\x00\x00\x00\x00", 11))},
             {"a context without an id", frame(13, std::string("\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00", 10))},
             {"two contexts of iTerm2", frame(13, std::string("\x02\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00", 19))},
             {"more contexts than the most", frame(13, varint(ShellContexts::MaxDepth + 1))},
             {"an extra cursor past the last column", frame(14, std::string(10, '\0') + "\x01\x00" + varint(Screen::MaxColumns) + "\x01")},
             {"a status state past Error", frame(12, std::string("\x01\x00\x05\x00\x01\x00\x00\x00\x01", 9))},
             {"a progress past 100", frame(12, std::string("\x01\x00\x01\x00\xca\x01\x00\x00\x00\x01", 10))},
             {"an extra cursor of shape 4", frame(14, std::string("\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x04", 14))},
             {"an extra cursor color mode 3", frame(14, std::string("\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 11))},
         }) {
        Terminal replica(20, 4, p.replicaHost);
        std::string why;
        EXPECT_FALSE(reader.apply(replica, bytes, &why)) << name;
        EXPECT_FALSE(why.empty()) << name;
    }
}

TEST(StateSync, NothingChangedIsNoFrame)
{
    Pair p(20, 4, 10);
    p.source.feed("x");
    ASSERT_TRUE(p.sync());
    EXPECT_EQ(p.update(), "");
    // One changed cell sends its row, not the screen.
    p.source.feed("y");
    const std::string frame = p.update();
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
    const std::string first = p.update();
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
        const std::string frame = p.update();
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

TEST(StateSync, RowsThatAFrameLeftOutGoFirstInTheNext)
{
    // The top rows change before every frame and fill its budget: a row
    // further down that changed once still goes in the next frame or so,
    // not only once the top rows rest.
    Pair p(30, 20, 0, 1500);
    std::string marks;
    for (int x = 0; x < 25; ++x) marks += "a\xcc\x81\xcc\x82\xcc\x83";
    ASSERT_TRUE(p.sync());
    p.source.feed("\x1b[20;1Hbottom");
    bool reached = false;
    for (int frame = 0; frame < 6 && !reached; ++frame) {
        for (int y = 1; y <= 10; ++y) p.source.feed("\x1b[" + std::to_string(y) + ";1H" + std::to_string(frame) + marks);
        const std::string update = p.update();
        std::string error;
        ASSERT_TRUE(p.reader.apply(p.replica, update, &error)) << error;
        reached = rowText(p.replica, 19).starts_with("bottom");
    }
    EXPECT_TRUE(reached);
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
        // What a frame carries besides the screens: shell marks, program
        // status, contexts, extra cursors, and scrolls of a region with
        // margins.
        "\x1b]133;A;click_events=1\x1b\\$ ", "\x1b]133;B\x1b\\", "\x1b]133;C\x1b\\", "\x1b]133;D;%d\x1b\\",
        "\x1b]133;D;-%d\x1b\\", "\x1b]7501;id=j%d:state=working:progress=%d\x1b\\", "\x1b]7501;state=done\x1b\\",
        "\x1b]7501;id=j%d:state=blocked:kind=question\x1b\\", "\x1b]3008;start=c%d;type=shell;user=u;hostname=h\x1b\\",
        "\x1b]3008;end=c%d\x1b\\", "\x1b]1337;RemoteHost=u@far%d\x07", "\x1b[>%d;2:%d:%d q", "\x1b[>0;4 q",
        "\x1b[>40;2:%d:0:0 q", "\x1b[?69h\x1b[2;%ds", "\x1b[?69l",
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
    const std::string frame = p.update();
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
    // A frame passes the budget by the bytes of its first row at most, and
    // never by the definitions of a row: its link counts before it goes.
    std::size_t largest = 0;
    for (int guard = 0; guard < 1000; ++guard) {
        const std::string frame = p.update();
        if (frame.empty() && !p.writer.pending()) break;
        largest = std::max(largest, frame.size());
        std::string error;
        ASSERT_TRUE(p.reader.apply(p.replica, frame, &error)) << error;
    }
    EXPECT_FALSE(p.writer.pending());
    EXPECT_EQ(differs(p.source, p.replica), "");
    EXPECT_LE(largest, p.budget + 512);
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

TEST(StateSync, MarginsRemoteHostsLargeExitCodesAndPendingWraps)
{
    Pair p(20, 6, 100);
    p.source.feed("\n\n\n\n\n\n\n\n");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    p.replicaHost.scrolls.clear();
    p.sourceHost.scrolls.clear();
    // A scroll within left and right margins moves part of each row.
    p.source.feed("\x1b[?69h\x1b[2;10s\x1b[2;4r\x1b[S\x1b[?69l\x1b[r");
    ASSERT_EQ(p.sourceHost.scrolls.size(), 1u);
    ASSERT_FALSE(p.sourceHost.scrolls[0].second.fullWidth);
    // An iTerm2 host, an exit code of two varint bytes (the terminal keeps
    // none below 0), and a mark one past the last column, after a full row.
    p.source.feed("\x1b]1337;RemoteHost=user@far\x07");
    p.source.feed("\x1b]133;A\x1b\\" + std::string(20, 'x') + "\x1b]133;B\x1b\\\r\n\x1b]133;C\x1b\\out\r\n\x1b]133;D;200\x1b\\");
    ASSERT_TRUE(p.sync(&error)) << error;
    ASSERT_EQ(p.replicaHost.scrolls.size(), 1u);
    const RegionScroll& theirs = p.sourceHost.scrolls[0].second;
    const RegionScroll& ours = p.replicaHost.scrolls[0].second;
    EXPECT_EQ(ours.left, theirs.left);
    EXPECT_EQ(ours.right, theirs.right);
    EXPECT_FALSE(ours.fullWidth);
    ASSERT_EQ(p.replica.contexts().stack().size(), 1u);
    EXPECT_TRUE(p.replica.contexts().stack()[0].iterm2RemoteHost);
    const auto regions = p.replica.semanticRegions();
    ASSERT_FALSE(regions.empty());
    EXPECT_EQ(regions.back().exitCode, std::optional<int>(200));
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, TheReplicaNumbersItsRowsAsTheWriterWhileTheHistoryCatchesUp)
{
    // A budget of a few rows: the history comes over many frames.
    Pair p(20, 4, 1000, 600);
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    for (int i = 0; i < 300; ++i) p.source.feed("line " + std::to_string(i) + "\r\n");
    p.source.feed("\x1b]133;A\x1b\\$ \x1b]133;B\x1b\\\x1b[1;3r\x1b[3;1H\n\x1b[r\x1b[4;1H");
    const auto firstVisible = [](const Terminal& t) {
        const Screen& s = t.primaryScreen();
        return s.rowsDropped() + s.historySize();
    };
    bool lagged = false;
    for (int guard = 0; guard < 10000; ++guard) {
        const std::string frame = p.update();
        if (frame.empty() && !p.writer.pending()) break;
        ASSERT_TRUE(p.reader.apply(p.replica, frame, &error)) << error;
        lagged = lagged || p.writer.pending();
        // Every row has the writer's id at every frame: the marks and the
        // scroll name the rows that they name in the source.
        ASSERT_EQ(firstVisible(p.replica), firstVisible(p.source)) << "frame " << guard;
    }
    EXPECT_TRUE(lagged);
    ASSERT_FALSE(p.replicaHost.scrolls.empty());
    EXPECT_EQ(p.replicaHost.scrolls.back().second.top, p.sourceHost.scrolls.back().second.top);
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, ANewLayoutNumbersTheReplicasRowsAnew)
{
    Pair p(30, 5, 100);
    p.source.feed("one\r\ntwo\r\n");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    // Narrower and back between two frames: the source's rows have new ids,
    // and the replica's size is the same as before.
    const std::uint64_t before = p.replica.primaryScreen().layoutEpoch();
    p.source.resize(25, 5, 0, 0);
    p.source.resize(30, 5, 0, 0);
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_NE(p.replica.primaryScreen().layoutEpoch(), before);
    // A change of height keeps every id: what the window keeps by id stays.
    const std::uint64_t kept = p.replica.primaryScreen().layoutEpoch();
    p.source.resize(30, 8, 0, 0);
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(p.replica.primaryScreen().layoutEpoch(), kept);
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, RowsOfLinksNeverMakeAFrameTooLarge)
{
    // Each cell its own link without an id: a row's definitions are far
    // more than the budget of a frame, on the visible rows of both screens
    // and on the newest history row.
    Pair p(60, 4, 100, 16 * 1024);
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    const auto heavyRow = [](char c) {
        std::string row;
        for (int i = 0; i < 40; ++i) row += "\x1b]8;;https://example.com/" + std::string(1500, c) + "\x1b\\" + c + "\x1b]8;;\x1b\\";
        return row;
    };
    p.source.feed("\x1b[?1049h" + heavyRow('z') + "\x1b[?1049l");
    for (int i = 0; i < 6; ++i) p.source.feed(heavyRow(char('a' + i)) + "\r\n");
    std::size_t largest = 0;
    for (int guard = 0; guard < 1000; ++guard) {
        const std::string frame = p.update();
        if (frame.empty() && !p.writer.pending()) break;
        largest = std::max(largest, frame.size());
        ASSERT_TRUE(p.reader.apply(p.replica, frame, &error)) << error;
    }
    EXPECT_FALSE(p.writer.pending());
    // The budget and one row's own bytes, not its links.
    EXPECT_LE(largest, p.budget + 1024);
    EXPECT_EQ(differs(p.source, p.replica), "");
    p.source.feed("\x1b[?1049h");
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
}

TEST(StateSync, AReplicaThatKeepsLessHistoryGetsNoOlderRows)
{
    const auto bytes = [](std::size_t replicaHistory) {
        Pair p(20, 4, 1000, 1 << 20, 10);
        p.writer.setReplicaHistory(replicaHistory);
        for (int i = 0; i < 500; ++i) p.source.feed("row " + std::to_string(i) + "\r\n");
        std::size_t total = 0;
        for (int guard = 0; guard < 1000; ++guard) {
            const std::string frame = p.update();
            if (frame.empty() && !p.writer.pending()) break;
            total += frame.size();
            std::string error;
            if (!p.reader.apply(p.replica, frame, &error)) return std::size_t(0);
        }
        // The replica keeps the newest rows either way.
        const Screen& s = p.replica.primaryScreen();
        if (s.historySize() != 10 || s.rowsDropped() + 10 != p.source.primaryScreen().rowsDropped() + p.source.primaryScreen().historySize()) {
            return std::size_t(0);
        }
        return total;
    };
    const std::size_t limited = bytes(10);
    const std::size_t all = bytes(~std::size_t(0));
    ASSERT_NE(limited, 0u);
    ASSERT_NE(all, 0u);
    EXPECT_LT(limited * 4, all);
}

TEST(StateSync, ThePromptRedrawOfAResizeReachesTheReplica)
{
    Pair p(30, 5, 100);
    p.source.feed("\x1b]133;A;redraw=1\x1b\\$ \x1b]133;B\x1b\\");
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    p.source.resize(25, 5, 0, 0);
    ASSERT_TRUE(p.source.promptRedrawPending());
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_TRUE(p.replica.promptRedrawPending());
    p.source.feed("\x1b]133;A;redraw=1\x1b\\$ \x1b]133;B\x1b\\");
    ASSERT_FALSE(p.source.promptRedrawPending());
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_FALSE(p.replica.promptRedrawPending());
}

TEST(StateSync, AFrameThatFailsLateTellsTheHostNothing)
{
    Pair p(20, 4, 10);
    ASSERT_TRUE(p.sync());
    p.source.feed("\x1b]7501;state=done\x1b\\\x1b]3008;start=a;type=shell\x1b\\");
    std::string frame = p.update();
    ASSERT_FALSE(frame.empty());
    // A section of no known type after the status and the contexts.
    frame += std::string("\x63\x00", 2);
    const int statusChanges = p.replicaHost.statusChanges;
    const int contextChanges = p.replicaHost.contextChanges;
    std::string error;
    EXPECT_FALSE(p.reader.apply(p.replica, frame, &error));
    EXPECT_EQ(p.replicaHost.statusChanges, statusChanges);
    EXPECT_EQ(p.replicaHost.contextChanges, contextChanges);
}

TEST(StateSync, BadModesLeaveTheModesAndTheScreen)
{
    Pair p(20, 4, 10);
    ASSERT_TRUE(p.sync());
    p.source.feed("\x1b[?1049h");
    const std::string frame = p.update();
    // The modes section first, its serial cut off.
    ASSERT_GE(frame.size(), 4u);
    ASSERT_EQ(frame[1], char(1));
    const std::size_t length = std::uint8_t(frame[2]);
    ASSERT_LT(length, 0x80u);
    std::string cut = frame.substr(0, 1) + char(1) + char(length - 1) + frame.substr(3, length - 1);
    std::string error;
    EXPECT_FALSE(p.reader.apply(p.replica, cut, &error));
    EXPECT_FALSE(p.replica.modes().altScreen);
    EXPECT_EQ(&p.replica.screen(), &p.replica.primaryScreen());
}

TEST(StateSync, TheReaderTellsOutputFromHistoryThatCatchesUp)
{
    Pair p(20, 4, 1000, 400);
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    for (int i = 0; i < 200; ++i) p.source.feed("line\r\n");
    ASSERT_TRUE(p.reader.apply(p.replica, p.update(), &error)) << error;
    EXPECT_TRUE(p.reader.screenChanged());
    ASSERT_TRUE(p.writer.pending());
    ASSERT_TRUE(p.reader.apply(p.replica, p.update(), &error)) << error;
    EXPECT_FALSE(p.reader.screenChanged());
}

TEST(StateSync, LinksWithoutAnIdStayApartOnTheReplica)
{
    // Each OSC 8 without an id is a link of its own, also with the same URI.
    // Rows that go, and new links in the places that their links freed, must
    // not make two of the writer's links one link of the replica's.
    Pair p(20, 4, 0);
    std::string error;
    const auto links = [](int count) {
        std::string row;
        for (int i = 0; i < count; ++i) row += "\x1b]8;;https://same\x1b\\x\x1b]8;;\x1b\\";
        return row;
    };
    p.source.feed(links(4) + "\r\n" + links(4));
    ASSERT_TRUE(p.sync(&error)) << error;
    for (int round = 0; round < 3; ++round) {
        p.source.feed("\x1b[1;1H\x1b[2K" + links(3) + "\x1b[2;1H\x1b[2K" + links(2));
        ASSERT_TRUE(p.sync(&error)) << error;
        // One writer link for each replica link, and the other way round.
        std::map<std::uint32_t, std::uint32_t> writerToReplica;
        std::map<std::uint32_t, std::uint32_t> replicaToWriter;
        const Screen& a = p.source.screen();
        const Screen& b = p.replica.screen();
        for (int y = 0; y < a.rows(); ++y) {
            for (int x = 0; x < a.cols(); ++x) {
                const std::uint32_t theirs = a.styles().get(a.row(y).at(x).styleId()).link;
                const std::uint32_t ours = b.styles().get(b.row(y).at(x).styleId()).link;
                if (!theirs) continue;
                ASSERT_NE(ours, 0u) << "round " << round << " row " << y << " cell " << x;
                EXPECT_EQ(writerToReplica.emplace(theirs, ours).first->second, ours) << "round " << round;
                EXPECT_EQ(replicaToWriter.emplace(ours, theirs).first->second, theirs) << "round " << round;
            }
        }
    }
}

TEST(StateSync, TheReaderTellsWhichRowsCame)
{
    // Output longer than the screen: it starts in the history and ends on
    // the screen.
    Pair p(20, 4, 1000, 300);
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    for (int i = 0; i < 200; ++i) p.source.feed("line " + std::to_string(i) + "\r\n");
    const auto firstVisible = [&] { return p.replica.primaryScreen().rowsDropped() + p.replica.primaryScreen().historySize(); };
    // While the history comes: the rows above the screen did not.
    ASSERT_TRUE(p.reader.apply(p.replica, p.update(), &error)) << error;
    ASSERT_TRUE(p.writer.pending());
    EXPECT_FALSE(p.reader.received(firstVisible() - 5, firstVisible() + 1));
    EXPECT_TRUE(p.reader.received(firstVisible(), firstVisible() + 1));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_TRUE(p.reader.received(firstVisible() - 5, firstVisible() + 1));
    EXPECT_TRUE(p.reader.received(p.replica.primaryScreen().rowsDropped(), firstVisible()));

    // A new replica gets the screen first, then the history newest first:
    // the older rows are still to come.
    p.writer.reset();
    Terminal late(20, 4, p.replicaHost, withScrollback(1000));
    StateReader reader;
    ASSERT_TRUE(reader.apply(late, p.writer.update(p.source, 300), &error)) << error;
    ASSERT_TRUE(p.writer.pending());
    const std::uint64_t oldest = p.source.primaryScreen().rowsDropped();
    const std::uint64_t visible = late.primaryScreen().rowsDropped() + late.primaryScreen().historySize();
    EXPECT_FALSE(reader.received(oldest, visible));
    EXPECT_TRUE(reader.received(visible, visible + 1));
    // Once the older rows came, all of them did.
    for (int guard = 0; guard < 1000 && p.writer.pending(); ++guard) {
        ASSERT_TRUE(reader.apply(late, p.writer.update(p.source, 300), &error)) << error;
    }
    ASSERT_FALSE(p.writer.pending());
    EXPECT_TRUE(reader.received(oldest, visible));

    // A replica that keeps less history than the writer has drops the older
    // rows as they come, and still hears of the last one.
    p.writer.reset();
    Terminal small(20, 4, p.replicaHost, withScrollback(50));
    StateReader smallReader;
    for (int guard = 0; guard < 1000; ++guard) {
        const std::string frame = p.writer.update(p.source, 300);
        if (frame.empty() && !p.writer.pending()) break;
        ASSERT_TRUE(smallReader.apply(small, frame, &error)) << error;
    }
    EXPECT_EQ(small.primaryScreen().historySize(), 50u);
    EXPECT_TRUE(smallReader.received(small.primaryScreen().rowsDropped(), visible));
}
