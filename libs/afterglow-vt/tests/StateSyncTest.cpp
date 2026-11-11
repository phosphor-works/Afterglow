// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The codec of the mux: a replica fed only by StateWriter frames must equal
// the terminal that parsed the bytes.

#include "TestUtil.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Placeholder.h>
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

/// A kitty graphics command (APC G).
std::string apc(std::string_view keys, std::string_view pixels = {})
{
    std::string s = "\x1b_G";
    s += keys;
    if (!pixels.empty()) {
        s += ';';
        s += AfterglowProto::base64Encode(pixels);
    }
    return s + "\x1b\\";
}

/// The RGBA pixels of a @p w by @p h image of one color.
std::string rgba(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    std::string px;
    for (int i = 0; i < w * h; ++i) px += {char(r), char(g), char(b), char(255)};
    return px;
}

/// A placeholder cell of image @p id (its low 8 bits in the 256-colour
/// foreground) at row @p row and column @p col of the image.
std::string placeholderCell(std::uint32_t id, int row, int col)
{
    const auto utf8 = [](char32_t cp) {
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
    };
    return "\x1b[38;5;" + std::to_string(id) + "m" + utf8(AfterglowProto::PlaceholderChar)
        + utf8(AfterglowProto::PlaceholderDiacritics[row]) + utf8(AfterglowProto::PlaceholderDiacritics[col]) + "\x1b[39m";
}

/// The first difference of the images and placements of both screens, and
/// of what the active screens draw, each at its own cell size, placeholder
/// cells included.
std::string imagesDiffer(const Terminal& a, const Terminal& b)
{
    for (const bool alternate : {false, true}) {
        const std::string screen = alternate ? "alternate " : "primary ";
        const GraphicsStore& x = a.graphics(alternate);
        const GraphicsStore& y = b.graphics(alternate);
        if (x.images().size() != y.images().size()) {
            return screen + "image count " + std::to_string(x.images().size()) + " and " + std::to_string(y.images().size());
        }
        for (const auto& [id, image] : x.images()) {
            const auto other = y.images().find(id);
            if (other == y.images().end()) return screen + "image " + std::to_string(id) + " is missing";
            const Image& o = other->second;
            if (image.number != o.number || image.isSixel != o.isSixel || image.anonymous != o.anonymous
                || image.transient != o.transient || image.width != o.width || image.height != o.height || image.gaps != o.gaps
                || image.order != o.order || image.sequence != o.sequence || image.frames.size() != o.frames.size()
                || image.animation != o.animation || image.maxLoops != o.maxLoops || image.loopsDone != o.loopsDone
                || image.currentFrame != o.currentFrame || image.stateCommands != o.stateCommands
                || image.frameCommands != o.frameCommands) {
                return screen + "image " + std::to_string(id);
            }
            for (std::size_t i = 0; i < image.frames.size(); ++i) {
                const ImageFrame& f = *image.frames[i];
                const ImageFrame& g = *o.frames[i];
                if (f.width != g.width || f.height != g.height || f.format != g.format || !(f.rgba == g.rgba) || !(f.indices == g.indices)) {
                    return screen + "frame " + std::to_string(i) + " of image " + std::to_string(id);
                }
            }
        }
        if (x.placements().size() != y.placements().size()) return screen + "placement count";
        for (std::size_t i = 0; i < x.placements().size(); ++i) {
            const Placement& p = x.placements()[i];
            const Placement& q = y.placements()[i];
            if (p.imageId != q.imageId || p.placementId != q.placementId || p.row != q.row || p.col != q.col
                || p.offsetX != q.offsetX || p.offsetY != q.offsetY || p.srcX != q.srcX || p.srcY != q.srcY || p.srcW != q.srcW
                || p.srcH != q.srcH || p.cols != q.cols || p.rows != q.rows || p.z != q.z || p.parentImage != q.parentImage
                || p.parentPlacement != q.parentPlacement || p.relativeX != q.relativeX || p.relativeY != q.relativeY
                || p.isVirtual != q.isVirtual || p.displayWidth != q.displayWidth || p.displayHeight != q.displayHeight) {
                return screen + "placement " + std::to_string(i);
            }
        }
    }
    // What each draws, at its own cell size: a cell size that did not come
    // draws elsewhere.
    if (a.cellWidth() != b.cellWidth() || a.cellHeight() != b.cellHeight()) return "cell size";
    const int cw = a.cellWidth() > 0 ? a.cellWidth() : 10;
    const int ch = a.cellHeight() > 0 ? a.cellHeight() : 20;
    const auto va = a.graphics().visible(a.screen(), cw, ch);
    const auto vb = b.graphics().visible(b.screen(), cw, ch);
    if (va.size() != vb.size()) return "visible count";
    for (std::size_t i = 0; i < va.size(); ++i) {
        if (va[i].imageId != vb[i].imageId || va[i].x != vb[i].x || va[i].y != vb[i].y || va[i].width != vb[i].width
            || va[i].height != vb[i].height || va[i].z != vb[i].z) {
            return "visible placement " + std::to_string(i);
        }
    }
    for (const auto& [id, image] : a.graphics().images()) {
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                const auto sa = a.graphics().placeholderSlice(id, 0, row, col, 0, 0, cw, ch);
                const auto sb = b.graphics().placeholderSlice(id, 0, row, col, 0, 0, cw, ch);
                if (sa.has_value() != sb.has_value()) return "placeholder slice of image " + std::to_string(id);
                if (sa && (sa->width != sb->width || sa->height != sb->height || sa->srcX != sb->srcX || sa->srcY != sb->srcY
                           || sa->srcW != sb->srcW || sa->srcH != sb->srcH || !(sa->frame->rgba == sb->frame->rgba))) {
                    return "placeholder slice of image " + std::to_string(id);
                }
            }
        }
    }
    return {};
}

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

TEST(StateSync, ImagesAndTheirPlacements)
{
    Pair p(30, 8, 100);
    p.source.resize(30, 8, 10, 20);
    p.source.feed(apc("a=T,f=32,s=2,v=2,i=7,q=2", rgba(2, 2, 255, 0, 0)));
    p.source.feed("\r\n" + apc("a=T,f=32,s=4,v=3,i=9,p=1,c=3,r=2,z=-1,q=2", rgba(4, 3, 0, 0, 255)));
    p.source.feed(apc("a=p,i=7,p=2,P=9,Q=1,H=2,V=1,q=2"));
    // An image that only placeholder cells show, and a sixel.
    p.source.feed(apc("a=T,f=32,s=1,v=1,i=12,U=1,q=2", rgba(1, 1, 0, 255, 0)));
    p.source.feed("\r\n" + placeholderCell(12, 0, 0));
    p.source.feed("\r\n\x1bPq#1;2;100;0;0#1~~\x1b\\");
    ASSERT_GE(p.source.graphics().images().size(), 4u);
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(differs(p.source, p.replica), "");
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    // A deleted image goes with its placements.
    p.source.feed(apc("a=d,d=I,i=9,q=2"));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    EXPECT_FALSE(p.replica.graphics().images().contains(9));
    // The alternate screen has images of its own.
    p.source.feed("\x1b[?1049h" + apc("a=T,f=32,s=1,v=1,i=3,q=2", rgba(1, 1, 9, 9, 9)));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    p.source.feed("\x1b[?1049l");
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    EXPECT_TRUE(p.replica.graphics().images().contains(7));
}

TEST(StateSync, AnImageLargerThanAFrameComesInParts)
{
    // 100 by 100 RGBA is 40000 bytes: frames of about 8 KiB.
    Pair p(30, 8, 100, 8192);
    p.source.resize(30, 8, 10, 20);
    p.source.feed(apc("a=T,f=32,s=100,v=100,i=50,q=2", rgba(100, 100, 1, 2, 3)));
    std::string frame = p.writer.update(p.source, p.budget, &p.scrolls);
    ASSERT_TRUE(p.reader.apply(p.replica, frame, nullptr));
    EXPECT_TRUE(p.writer.pending());
    // No image shows before all of its pixels came.
    EXPECT_TRUE(p.replica.graphics().images().empty());
    EXPECT_TRUE(p.replica.graphics().placements().empty());
    // An image of a lower id that comes meanwhile waits for the first.
    p.source.feed(apc("a=T,f=32,s=2,v=2,i=3,q=2", rgba(2, 2, 4, 5, 6)));
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_GT(p.frames, 4);
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    // An image deleted before all of it came: the next one comes all the same.
    p.source.feed(apc("a=T,f=32,s=100,v=100,i=60,q=2", rgba(100, 100, 7, 8, 9)));
    frame = p.writer.update(p.source, p.budget, &p.scrolls);
    ASSERT_TRUE(p.reader.apply(p.replica, frame, nullptr));
    p.source.feed(apc("a=d,d=I,i=60,q=2") + apc("a=T,f=32,s=90,v=90,i=61,q=2", rgba(90, 90, 1, 1, 1)));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    EXPECT_FALSE(p.replica.graphics().images().contains(60));
    EXPECT_TRUE(p.replica.graphics().images().contains(61));
}

TEST(StateSync, AnAnimationRunsOnTheReplicasClock)
{
    Pair p(30, 8, 100);
    p.source.resize(30, 8, 10, 20);
    p.source.feed(apc("a=T,f=32,s=2,v=2,i=10,q=2", rgba(2, 2, 255, 0, 0)));
    p.source.feed(apc("a=f,f=32,s=2,v=2,i=10,z=100,q=2", rgba(2, 2, 0, 255, 0)));
    p.source.feed(apc("a=a,i=10,r=1,z=100,q=2") + apc("a=a,i=10,s=3,q=2"));
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    // The cell size that the window measured comes with the frames: the
    // replica draws and animates the images at it.
    ASSERT_EQ(p.replica.cellWidth(), 10);
    ASSERT_EQ(p.replica.cellHeight(), 20);
    const Image& image = p.replica.graphics().images().at(10);
    ASSERT_EQ(image.animation, Image::Animation::Running);
    p.replica.advanceAnimations(1000);
    p.replica.advanceAnimations(1150);
    ASSERT_EQ(image.currentFrame, 1u);
    // Output and a new frame change no step of the animation.
    p.source.feed("x" + apc("a=f,f=32,s=2,v=2,i=10,z=100,q=2", rgba(2, 2, 0, 0, 255)));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(p.replica.graphics().images().at(10).frames.size(), 3u);
    EXPECT_EQ(p.replica.graphics().images().at(10).currentFrame, 1u);
    EXPECT_EQ(p.replica.graphics().images().at(10).animation, Image::Animation::Running);
    // A command that stops it does: on the frame that shows, as a local
    // terminal stops it.
    p.source.feed(apc("a=a,i=10,s=1,q=2"));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(p.replica.graphics().images().at(10).animation, Image::Animation::Stopped);
    EXPECT_EQ(p.replica.graphics().images().at(10).currentFrame, 1u);
    // A frame that a command names shows, also the one that the writer's
    // store already names: its animation never moved.
    p.source.feed(apc("a=a,i=10,c=1,q=2"));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(p.replica.graphics().images().at(10).currentFrame, 0u);
}

TEST(StateSync, AFinishedAnimationPlaysAgainForItsCommand)
{
    Pair p(30, 8, 100);
    p.source.resize(30, 8, 10, 20);
    p.source.feed(apc("a=T,f=32,s=2,v=2,i=10,q=2", rgba(2, 2, 255, 0, 0)));
    p.source.feed(apc("a=f,f=32,s=2,v=2,i=10,z=100,q=2", rgba(2, 2, 0, 255, 0)));
    p.source.feed(apc("a=a,i=10,r=1,z=100,q=2") + apc("a=a,i=10,s=3,v=2,q=2"));
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    // One loop, on the replica's clock: it stays on its last frame.
    for (std::uint64_t now = 1000; now <= 2000; now += 50) p.replica.advanceAnimations(now);
    ASSERT_EQ(p.replica.graphics().images().at(10).loopsDone, 1u);
    // The same command again, which changes no value of the writer's store:
    // the animation plays again.
    p.source.feed(apc("a=a,i=10,s=3,q=2"));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(p.replica.graphics().images().at(10).loopsDone, 0u);
    // A new image under the id starts over, whatever its animation's values.
    for (std::uint64_t now = 3000; now <= 4000; now += 50) p.replica.advanceAnimations(now);
    ASSERT_EQ(p.replica.graphics().images().at(10).loopsDone, 1u);
    p.source.feed(apc("a=T,f=32,s=2,v=2,i=10,q=2", rgba(2, 2, 0, 0, 255)));
    p.source.feed(apc("a=f,f=32,s=2,v=2,i=10,z=100,q=2", rgba(2, 2, 255, 255, 0)));
    p.source.feed(apc("a=a,i=10,s=3,v=2,q=2"));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(p.replica.graphics().images().at(10).loopsDone, 0u);
    EXPECT_EQ(p.replica.graphics().images().at(10).currentFrame, 0u);
}

TEST(StateSync, ADeletedFrameKeepsTheFrameThatShows)
{
    Pair p(30, 8, 100);
    p.source.resize(30, 8, 10, 20);
    p.source.feed(apc("a=T,f=32,s=2,v=2,i=10,q=2", rgba(2, 2, 255, 0, 0)));
    for (const char* color : {"green", "blue"}) {
        p.source.feed(apc("a=f,f=32,s=2,v=2,i=10,z=100,q=2",
                          std::string_view(color) == "green" ? rgba(2, 2, 0, 255, 0) : rgba(2, 2, 0, 0, 255)));
    }
    p.source.feed(apc("a=a,i=10,c=3,q=2"));
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    const FramePtr third = p.replica.graphics().images().at(10).frames[2];
    // The first frame goes: the replica keeps showing the third, now second.
    p.source.feed(apc("a=d,d=f,i=10,r=1,q=2"));
    ASSERT_TRUE(p.sync(&error)) << error;
    const Image& image = p.replica.graphics().images().at(10);
    ASSERT_EQ(image.frames.size(), 2u);
    EXPECT_EQ(image.currentFrame, 1u);
    EXPECT_EQ(image.frames[image.currentFrame], third);
}

TEST(StateSync, AnImageSentAgainFasterThanItsPixelsGoStillShows)
{
    // 100 by 100 RGBA is 40000 bytes: at 8 KiB a frame, five frames for one.
    Pair p(30, 8, 100, 8192);
    p.source.resize(30, 8, 10, 20);
    std::string error;
    for (int version = 0; version < 20; ++version) {
        // A new image under the id before each frame of the codec.
        p.source.feed(apc("a=T,f=32,s=100,v=100,i=1,q=2", rgba(100, 100, std::uint8_t(version), 0, 0)));
        const std::string frame = p.writer.update(p.source, p.budget, &p.scrolls);
        ASSERT_TRUE(p.reader.apply(p.replica, frame, &error)) << error;
    }
    // A version that the writer began finished, and shows.
    ASSERT_TRUE(p.replica.graphics().images().contains(1));
    EXPECT_EQ(p.replica.graphics().images().at(1).width, 100u);
    // Once the program stops, the last version comes, and the replica drops
    // the frames of the others.
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    EXPECT_EQ(p.replica.graphics().images().at(1).frames[0]->rgba[0], 19);
    EXPECT_EQ(p.reader.frames(), 1u);
}

TEST(StateSync, AnImageThatAFrameDeletionCompletesShowsAtOnceAndStaysNewest)
{
    // Three frames of 40000 bytes, at 8 KiB a frame of the codec.
    Pair p(30, 8, 100, 8192);
    p.source.resize(30, 8, 10, 20);
    p.source.feed(apc("a=T,f=32,s=100,v=100,i=1,q=2", rgba(100, 100, 1, 0, 0)));
    for (const std::uint8_t red : {2, 3}) p.source.feed(apc("a=f,f=32,s=100,v=100,i=1,q=2", rgba(100, 100, red, 0, 0)));
    std::string error;
    ASSERT_TRUE(p.reader.apply(p.replica, p.update(), &error)) << error;
    ASSERT_TRUE(p.writer.pending());
    // The third frame goes while the first goes to the replica: the version
    // that the writer sends keeps its three frames, and the store's image is
    // whole once two of them went. It shows then, and the third frame's
    // pixels stop.
    p.source.feed(apc("a=d,d=f,i=1,r=3,q=2"));
    // Each frame of the codec: the replica never shows the deleted frame.
    const auto apply = [&](const std::string& frame) {
        ASSERT_TRUE(p.reader.apply(p.replica, frame, &error)) << error;
        const auto& images = p.replica.graphics().images();
        if (const auto it = images.find(1); it != images.end()) {
            for (const FramePtr& f : it->second.frames) ASSERT_NE(f->rgba[0], 3);
        }
    };
    int shown = -1;
    int frames = 0;
    bool caughtUp = false;
    for (int guard = 0; guard < 1000 && !caughtUp; ++guard) {
        const std::string frame = p.update();
        caughtUp = frame.empty() && !p.writer.pending();
        if (caughtUp) break;
        ++frames;
        apply(frame);
        if (HasFatalFailure()) return;
        // No pixels of the third frame go once the first two went.
        if (shown < 0) {
            EXPECT_LE(p.reader.frames(), 2u) << frames;
        }
        if (shown < 0 && p.replica.graphics().images().contains(1)) {
            shown = frames;
            // A new frame comes while the writer would still send the third.
            p.source.feed(apc("a=f,f=32,s=100,v=100,i=1,q=2", rgba(100, 100, 4, 0, 0)));
        }
    }
    ASSERT_TRUE(caughtUp);
    ASSERT_GE(shown, 0);
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    ASSERT_EQ(p.replica.graphics().images().at(1).frames.size(), 3u);
    EXPECT_EQ(p.replica.graphics().images().at(1).frames[2]->rgba[0], 4);
    // The replica dropped the part of the third frame that it got.
    EXPECT_EQ(p.reader.frames(), 3u);
}

TEST(StateSync, AVersionThatShowedLeavesTheReplicaOnceTheStoresImageShows)
{
    // Two frames of 40000 bytes, at 8 KiB a frame of the codec.
    Pair p(30, 8, 100, 8192);
    p.source.resize(30, 8, 10, 20);
    p.source.feed(apc("a=T,f=32,s=100,v=100,i=1,q=2", rgba(100, 100, 1, 0, 0))
                  + apc("a=f,f=32,s=100,v=100,i=1,q=2", rgba(100, 100, 2, 0, 0)));
    std::string error;
    ASSERT_TRUE(p.reader.apply(p.replica, p.update(), &error)) << error;
    ASSERT_TRUE(p.writer.pending());
    // The second frame goes, and a third takes its place, while the first
    // goes: the version that goes shows first, then the store's.
    p.source.feed(apc("a=d,d=f,i=1,r=2,q=2") + apc("a=f,f=32,s=100,v=100,i=1,q=2", rgba(100, 100, 3, 0, 0)));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    EXPECT_EQ(p.replica.graphics().images().at(1).frames[1]->rgba[0], 3);
    // The replica keeps only the frames of the store's image.
    EXPECT_EQ(p.reader.frames(), 2u);
}

TEST(StateSync, ReaderRefusesBadImageSections)
{
    SyncHost host;
    const auto section = [](std::uint8_t tag, const std::string& payload) {
        return std::string(1, char(tag)) + varint(payload.size()) + payload;
    };
    // The first part of frame @p id of @p w by @p h RGBA, with @p bytes of
    // pixels.
    const auto start = [&](std::uint64_t id, std::uint64_t w, std::uint64_t h, std::size_t bytes) {
        return section(16, varint(id) + varint(0) + varint(w) + varint(h) + std::string(1, '\0') + std::string(bytes, '\xff'));
    };
    const std::string frame1 = start(1, 1, 1, 4);
    // Image 9 of @p width by 1 on the primary screen, with the frames and
    // the gaps given.
    const auto image = [&](std::vector<std::uint64_t> frames, std::vector<std::uint64_t> gaps, std::uint64_t width = 1) {
        std::string record = varint(9) + varint(0) + std::string(1, '\0') + varint(width) + varint(1) + varint(frames.size());
        for (const std::uint64_t id : frames) record += varint(id);
        record += varint(gaps.size());
        for (const std::uint64_t gap : gaps) record += varint(gap);
        record += std::string(1, '\0') + varint(0) + varint(0) + varint(0) + varint(0) + varint(0) + varint(1) + varint(1);
        return section(17, std::string(1, '\0') + varint(0) + varint(1) + record);
    };
    // A placement of image @p id at stable row @p row.
    const auto placement = [&](std::uint64_t id, std::uint64_t row) {
        std::string p = varint(id) + varint(1) + varint(row) + std::string(14, '\0') + std::string(1, '\0') + varint(0) + varint(0);
        return section(18, std::string(1, '\0') + varint(1) + p);
    };
    const std::string version(1, char(1));
    // The builders make what a reader takes.
    {
        StateReader reader;
        Terminal replica(20, 4, host);
        std::string why;
        ASSERT_TRUE(reader.apply(replica, version + frame1 + image({1}, {40}) + placement(9, 0), &why)) << why;
        EXPECT_EQ(replica.graphics().placements().size(), 1u);
    }
    for (const auto& [name, bytes] : std::vector<std::pair<const char*, std::string>>{
             {"a part of a frame that did not start", section(16, varint(1) + varint(4) + std::string(4, '\xff'))},
             {"a frame of no width", start(1, 0, 1, 4)},
             {"a frame of format 3", section(16, varint(1) + varint(0) + varint(1) + varint(1) + std::string(1, '\3') + "x")},
             {"more pixels than the frame holds", start(1, 1, 1, 8)},
             {"a frame that starts while another builds", start(1, 2, 2, 4) + start(2, 1, 1, 4)},
             {"a frame id twice", frame1 + frame1},
             {"a part after its frame was dropped", start(1, 2, 2, 4) + section(19, varint(1) + varint(1)) + section(16, varint(1) + varint(4) + std::string(4, '\xff'))},
             {"an image with a frame that did not come", image({5}, {40})},
             {"an image without frames", frame1 + image({}, {})},
             {"an image without a gap for each frame", frame1 + image({1}, {})},
             {"an image with a frame of another size", frame1 + image({1}, {40}, 2)},
             {"a gap past what a command gives", frame1 + image({1}, {0x80000000u})},
             {"a placement of no image", placement(5, 0)},
             {"a placement row past what a row takes", frame1 + image({1}, {40}) + placement(9, std::uint64_t(1) << 62)},
         }) {
        // A reader of its own: a refused frame leaves a reader in no state
        // to judge the next.
        StateReader reader;
        Terminal replica(20, 4, host);
        std::string why;
        EXPECT_FALSE(reader.apply(replica, version + bytes, &why)) << name;
        EXPECT_FALSE(why.empty()) << name;
    }
}

TEST(StateSync, AReaderTakesAWriterThatStartsOver)
{
    Pair p(30, 8, 100);
    p.source.resize(30, 8, 10, 20);
    p.source.feed(apc("a=T,f=32,s=2,v=2,i=7,q=2", rgba(2, 2, 255, 0, 0)));
    std::string error;
    ASSERT_TRUE(p.sync(&error)) << error;
    // The same reader, a new writer: its frame ids start over, and the images
    // of the old writer go.
    p.writer.reset();
    p.source.feed(apc("a=d,d=I,i=7,q=2") + apc("a=T,f=32,s=2,v=2,i=8,q=2", rgba(2, 2, 0, 0, 255)));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    EXPECT_FALSE(p.replica.graphics().images().contains(7));
}

TEST(StateSync, ManyImagesComeInFramesThatFit)
{
    // 200 images of 64 frames each: their records are far more than a state
    // frame of 8 KiB, and so are the frames that go when the images do.
    Pair p(30, 8, 100, 8192);
    p.source.resize(30, 8, 10, 20);
    for (int id = 1; id <= 200; ++id) {
        p.source.feed(apc("a=t,f=32,s=1,v=1,i=" + std::to_string(id) + ",q=2", rgba(1, 1, 1, 2, 3)));
        for (int k = 1; k < 64; ++k) p.source.feed(apc("a=f,f=32,s=1,v=1,i=" + std::to_string(id) + ",q=2", rgba(1, 1, 4, 5, 6)));
    }
    std::string error;
    std::size_t largest = 0;
    const auto drain = [&] {
        for (int guard = 0; guard < 100000; ++guard) {
            const std::string frame = p.update();
            if (frame.empty() && !p.writer.pending()) return true;
            largest = std::max(largest, frame.size());
            if (!p.reader.apply(p.replica, frame, &error)) return false;
        }
        return false;
    };
    ASSERT_TRUE(drain()) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    ASSERT_EQ(p.replica.graphics().images().size(), 200u);
    // A command to each image at once: 200 records again.
    for (int id = 1; id <= 200; ++id) p.source.feed(apc("a=a,i=" + std::to_string(id) + ",c=2,q=2"));
    ASSERT_TRUE(drain()) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    // Every image goes: 12,800 frames to drop.
    for (int id = 1; id <= 200; ++id) p.source.feed(apc("a=d,d=I,i=" + std::to_string(id) + ",q=2"));
    ASSERT_TRUE(drain()) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    EXPECT_TRUE(p.replica.graphics().images().empty());
    EXPECT_LT(largest, std::size_t(8192 + 4096));
}

TEST(StateSync, ATransferThatEndsDropsTheFrameThatTheReplicaBuilds)
{
    // A sixel of 200 by 200: its pixels go in parts of 8 KiB.
    Pair p(30, 8, 100, 8192);
    p.source.resize(30, 8, 10, 20);
    std::string sixel = "\x1bPq\"1;1;200;200#1;2;100;0;0#1";
    for (int band = 0; band < 34; ++band) sixel += "!200~-";
    p.source.feed(sixel + "\x1b\\");
    std::uint32_t id = 0;
    for (const auto& [key, image] : p.source.graphics().images()) {
        if (image.isSixel) id = key;
    }
    ASSERT_NE(id, 0u);
    std::string error;
    std::string frame = p.update();
    ASSERT_TRUE(p.reader.apply(p.replica, frame, &error)) << error;
    ASSERT_TRUE(p.writer.pending());
    // A kitty image takes the sixel's id, which moves the sixel with its
    // frames to another id, and goes. The frame that the replica builds is
    // still the moved sixel's, and the next image starts.
    const std::string named = "i=" + std::to_string(id);
    p.source.feed(apc("a=t,f=32,s=1,v=1," + named + ",q=2", rgba(1, 1, 1, 1, 1)) + apc("a=d,d=I," + named + ",q=2")
                  + apc("a=T,f=32,s=1,v=1,i=1,q=2", rgba(1, 1, 2, 2, 2)));
    ASSERT_TRUE(p.sync(&error)) << error;
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
}

TEST(StateSync, ATransferOfAnImageThatAClearReplacedEnds)
{
    Pair p(30, 8, 100, 8192);
    p.source.resize(30, 8, 10, 20);
    p.source.feed("\x1b[?1049h" + apc("a=T,f=32,s=100,v=100,i=1,q=2", rgba(100, 100, 1, 2, 3)));
    std::string error;
    ASSERT_TRUE(p.reader.apply(p.replica, p.update(), &error)) << error;
    ASSERT_TRUE(p.writer.pending());
    // The alternate screen goes and comes again with another image under
    // the id: the replica shows that one, never the one that the clear took.
    p.source.feed("\x1b[?1049l\x1b[?1049h" + apc("a=T,f=32,s=100,v=100,i=1,q=2", rgba(100, 100, 7, 8, 9)));
    bool caughtUp = false;
    for (int guard = 0; guard < 1000 && !caughtUp; ++guard) {
        const std::string frame = p.update();
        caughtUp = frame.empty() && !p.writer.pending();
        if (caughtUp) break;
        ASSERT_TRUE(p.reader.apply(p.replica, frame, &error)) << error;
        const auto& images = p.replica.graphics(true).images();
        if (const auto it = images.find(1); it != images.end()) {
            ASSERT_FALSE(it->second.frames.empty());
            ASSERT_EQ(it->second.frames[0]->rgba[0], 7);
        }
    }
    ASSERT_TRUE(caughtUp);
    EXPECT_EQ(imagesDiffer(p.source, p.replica), "");
    EXPECT_EQ(p.replica.graphics().images().at(1).frames[0]->rgba[0], 7);
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
    // Images too, with their placements, frames and animations, which those
    // move, erase and reset.
    const std::string pixels = AfterglowProto::base64Encode(rgba(2, 2, 10, 20, 30));
    // Larger than the state frames of every run: it comes in parts.
    const std::string large = AfterglowProto::base64Encode(rgba(40, 40, 40, 50, 60));
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
        "\x1b_Ga=T,f=32,s=2,v=2,i=%d,q=2;" + pixels + "\x1b\\", "\x1b_Ga=p,i=%d,q=2\x1b\\", "\x1b_Ga=d,d=I,i=%d,q=2\x1b\\",
        "\x1b_Ga=T,f=32,s=40,v=40,i=%d,q=2;" + large + "\x1b\\",
        "\x1b_Ga=f,f=32,s=2,v=2,i=%d,q=2;" + pixels + "\x1b\\", "\x1b_Ga=a,i=%d,s=3,q=2\x1b\\", "\x1b_Ga=d,d=A,q=2\x1b\\",
        "\x1bPq#1;2;100;0;0#1~~\x1b\\",
    };
    for (unsigned seed = 1; seed <= 60; ++seed) {
        std::mt19937 rng(seed);
        Pair p(20 + int(rng() % 40), 4 + int(rng() % 12), rng() % 3 ? 60 : 0, 256 + rng() % 4096);
        // A cell size, as a window measures it: images need one.
        p.source.resize(p.source.cols(), p.source.rows(), 10, 20);
        for (int step = 0; step < 1500; ++step) {
            std::string piece = pieces[rng() % pieces.size()];
            for (std::size_t at; (at = piece.find("%d")) != std::string::npos;) piece.replace(at, 2, std::to_string(rng() % 20));
            p.source.feed(piece);
            if (rng() % 5 == 0) {
                // One frame, which may leave the pixels of an image half sent
                // when the next output replaces it.
                std::string error;
                ASSERT_TRUE(p.reader.apply(p.replica, p.update(), &error)) << "seed " << seed << " step " << step << ": " << error;
            }
            if (rng() % 9 == 0) {
                std::string error;
                ASSERT_TRUE(p.sync(&error)) << "seed " << seed << " step " << step << ": " << error;
                const std::string d = differs(p.source, p.replica);
                ASSERT_TRUE(d.empty()) << "seed " << seed << " step " << step << ": " << d;
                const std::string images = imagesDiffer(p.source, p.replica);
                ASSERT_TRUE(images.empty()) << "seed " << seed << " step " << step << ": " << images;
            }
            if (step == 700) p.source.resize(15 + int(rng() % 40), 3 + int(rng() % 10), 10, 20);
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
