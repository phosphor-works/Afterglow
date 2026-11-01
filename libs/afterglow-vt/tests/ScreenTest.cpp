// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

#include <AfterglowVt/Selection.h>
#include <AfterglowVt/TextSearch.h>

#include <gtest/gtest.h>

#include <atomic>
#include <random>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {
struct Term
{
    RecordingHost host;
    Terminal t;
    Term(int cols, int rows)
        : t(cols, rows, host)
    {
    }
};
} // namespace

TEST(Reflow, ShrinkWrapsLongLine)
{
    Term t(10, 3);
    t.t.feed("0123456789AB");
    EXPECT_EQ(rowText(t.t, 0), "0123456789");
    t.t.resize(5, 3, 8, 16);
    EXPECT_EQ(screenText(t.t), "01234\n56789\nAB");
    EXPECT_EQ(t.t.screen().cursor.y, 2);
    EXPECT_EQ(t.t.screen().cursor.x, 2);
}

TEST(Reflow, GrowUnwraps)
{
    Term t(5, 3);
    t.t.feed("0123456789AB");
    t.t.resize(12, 3, 8, 16);
    EXPECT_EQ(rowText(t.t, 0), "0123456789AB");
    EXPECT_EQ(t.t.screen().cursor.x, 11);
    EXPECT_EQ(t.t.screen().cursor.y, 0);
}

TEST(Reflow, HardLineBreaksStay)
{
    Term t(10, 4);
    t.t.feed("abc\r\ndef\r\n$ ");
    t.t.resize(4, 4, 8, 16);
    EXPECT_EQ(screenText(t.t), "abc\ndef\n$");
    EXPECT_EQ(t.t.screen().cursor.y, 2);
    EXPECT_EQ(t.t.screen().cursor.x, 2);
}

TEST(Reflow, WideCharMovesToNextRow)
{
    Term t(6, 3);
    t.t.feed("abc\xE4\xB8\xAD" "d");
    t.t.resize(4, 3, 8, 16);
    EXPECT_EQ(rowText(t.t, 0), "abc");
    EXPECT_TRUE(t.t.screen().row(0).cells[3].isSpacerHead());
    EXPECT_EQ(rowText(t.t, 1), "\xE4\xB8\xAD" "d");
}

TEST(Reflow, OverflowGoesToHistory)
{
    Term t(10, 2);
    t.t.feed("0123456789\r\n$ ");
    t.t.resize(5, 2, 8, 16);
    EXPECT_EQ(t.t.screen().historySize(), 1u);
    EXPECT_EQ(rowText(t.t, 0), "56789");
    EXPECT_EQ(rowText(t.t, 1), "$");
}

TEST(Resize, AltScreenTruncates)
{
    Term t(6, 3);
    t.t.feed("\x1b[?1049habcdef");
    t.t.resize(3, 3, 8, 16);
    EXPECT_EQ(rowText(t.t, 0), "abc");
}

TEST(Viewport, ScrollbackPinsWhileOutputArrives)
{
    Term t(5, 2);
    t.t.feed("1\r\n2\r\n3\r\n4");
    t.t.scrollViewport(1);
    const std::size_t top = t.t.screen().viewportTop();
    t.t.feed("\r\n5");
    EXPECT_EQ(t.t.screen().viewportTop(), top);
}

namespace {
// What fish does on SIGWINCH: CR, cursor up by the prompt height it drew at the
// old width, clear to end of screen, then repaint the prompt.
void fishRepaint(Terminal& t, int promptRowsAbove, std::string_view prompt)
{
    std::string s = "\r";
    for (int i = 0; i < promptRowsAbove; ++i) {
        s += "\x1b[A";
    }
    s += "\x1b[K\x1b]133;A\x1b\\\x1b[J";
    s += prompt;
    t.feed(s);
}
} // namespace

TEST(Reflow, RedrawablePromptKeepsItsPlace)
{
    Term t(20, 8);
    // Output, then a 3-row prompt (blank line, full-width top line, input line).
    const std::string prompt = "\r\n01234567890123456789\r\n> ";
    t.t.feed("out1\r\nout2\r\n\x1b]133;A\x1b\\");
    t.t.feed(prompt);
    t.t.feed("\x1b]133;B\x1b\\");
    ASSERT_EQ(t.t.screen().cursor.y, 4);

    for (int cols : {15, 25, 12, 20, 30, 20}) {
        t.t.resize(cols, 8, 8, 16);
        // The prompt region still spans 3 rows ending at the cursor, so fish's
        // "up 2" lands on the prompt start.
        const int cy = t.t.screen().cursor.y;
        ASSERT_GE(cy, 2);
        EXPECT_TRUE(t.t.screen().row(cy - 2).promptStart) << "cols " << cols;
        const std::string top = std::string("01234567890123456789").substr(0, std::size_t(cols));
        fishRepaint(t.t, 2, "\r\n" + top + "\r\n> ");
        t.t.feed("\x1b]133;B\x1b\\");
        EXPECT_EQ(rowText(t.t, 0), "out1") << "cols " << cols;
        EXPECT_EQ(rowText(t.t, 1), "out2") << "cols " << cols;
        EXPECT_EQ(rowText(t.t, 2), "") << "cols " << cols;
        EXPECT_EQ(rowText(t.t, 3), top) << "cols " << cols;
        EXPECT_EQ(rowText(t.t, 4), ">") << "cols " << cols;
        EXPECT_EQ(t.t.screen().cursor.y, 4) << "cols " << cols;
    }
}

TEST(Reflow, PromptOptOutReflowsNormally)
{
    Term t(10, 4);
    t.t.feed("\x1b]133;A;redraw=0\x1b\\0123456789AB");
    t.t.resize(5, 4, 8, 16);
    EXPECT_EQ(screenText(t.t), "01234\n56789\nAB");
}

TEST(Reflow, PromptSurvivesResizeWithoutShellRepaint)
{
    Term t(20, 8);
    t.t.feed("fastfetch output\r\n\x1b]133;A\x1b\\\r\nuser@host\r\n> \x1b]133;B\x1b\\typed");
    // Fish may only query the cursor after SIGWINCH: its screen diff can
    // leave unchanged prompt/input cells alone. No Enter or full repaint.
    for (int cols : {30, 18, 32, 22}) {
        t.t.resize(cols, 8, 8, 16);
        t.t.feed("\x1b[6n\x1b[0c");
        const int cy = t.t.screen().cursor.y;
        EXPECT_EQ(rowText(t.t, cy - 1), "user@host");
        EXPECT_EQ(rowText(t.t, cy), "> typed");
        EXPECT_EQ(rowText(t.t, 0), "fastfetch output");
        EXPECT_EQ(t.t.screen().cursor.x, 7);
    }
    t.t.feed("!"); // incremental input update still uses the retained row
    EXPECT_EQ(rowText(t.t, t.t.screen().cursor.y), "> typed!");
}

TEST(Reflow, PreservedPromptClipsWideGlyphsAndRetainsStyles)
{
    Term t(12, 6);
    t.t.feed("\x1b]133;A\x1b\\\x1b[31me\xCC\x81" "ab\xE4\xB8\xAD\xCC\x81"
             "\x1b[32mtail\x1b[0m\r\n> \x1b]133;B\x1b\\");
    const auto style = t.t.screen().styles().get(t.t.screen().row(0).at(0).styleId());
    ASSERT_EQ(t.t.screen().styles().liveCount(), 3u);
    // The new last column bisects the wide character and its combining mark.
    t.t.resize(4, 6, 8, 16);
    EXPECT_EQ(rowText(t.t, 0), "e\xCC\x81" "ab");
    EXPECT_EQ(rowText(t.t, 1), ">");
    EXPECT_TRUE(t.t.screen().row(0).at(3).isEmpty());
    EXPECT_FALSE(t.t.screen().row(0).at(3).hasGrapheme());
    EXPECT_EQ(t.t.screen().graphemeExtra(t.t.screen().row(0), 3), U"");
    EXPECT_EQ(t.t.screen().styles().liveCount(), 2u); // green clipped, red retained
    t.t.resize(16, 6, 8, 16);
    EXPECT_EQ(rowText(t.t, 0), "e\xCC\x81" "ab");
    // Reuse the released style slot; the surviving prompt must stay red.
    t.t.feed("\x1b[34mnew\x1b[0m");
    EXPECT_EQ(t.t.screen().styles().get(t.t.screen().row(0).at(0).styleId()), style);
    t.t.feed("\x1b[2J");
    EXPECT_EQ(t.t.screen().styles().liveCount(), 1u);
}

TEST(Reflow, CommandOutputIsNeverBlanked)
{
    Term t(10, 4);
    t.t.feed("\x1b]133;A\x1b\\$ ls\x1b]133;C\x1b\\\r\n0123456789AB");
    t.t.resize(5, 6, 8, 16);
    EXPECT_EQ(screenText(t.t), "$ ls\n01234\n56789\nAB");
}

TEST(Reflow, PromptRedrawPendingUntilShellRepaints)
{
    Term t(20, 6);
    t.t.feed("\x1b]133;A\x1b\\> \x1b]133;B\x1b\\");
    EXPECT_FALSE(t.t.promptRedrawPending());
    t.t.resize(20, 8, 8, 16); // height only: no reflow
    EXPECT_FALSE(t.t.promptRedrawPending());
    t.t.resize(15, 8, 8, 16);
    EXPECT_TRUE(t.t.promptRedrawPending());
    t.t.feed("\r\x1b]133;A\x1b\\\x1b[J> ");
    EXPECT_TRUE(t.t.promptRedrawPending());
    t.t.feed("\x1b]133;B\x1b\\");
    EXPECT_FALSE(t.t.promptRedrawPending());
}

TEST(History, RowsAreCompactedAndStillReadable)
{
    RecordingHost host;
    Terminal t(80, 4, host);
    for (int i = 0; i < 50; ++i) {
        t.feed("line " + std::to_string(i) + "\r\n");
    }
    const Screen& s = t.screen();
    ASSERT_EQ(s.historySize(), 47u);
    // "line NN" needs at most 7 cells of the 80 stored for a full row.
    EXPECT_LE(s.historyCells(), s.historySize() * 7);
    EXPECT_EQ(rowText(s, s.absoluteRow(10)), "line 10");

    // Search and selection read compacted rows.
    TextSnapshot text;
    for (std::size_t i = 0; i < s.totalRows(); ++i) {
        appendTextRow(text, s, i);
    }
    const std::atomic<bool> cancelled{false};
    EXPECT_EQ(searchText(text, U"line 1", false, cancelled).matches.size(), 11u); // 1, 10-19
    Selection sel;
    sel.anchor = {s.rowsDropped() + 3, 0};
    sel.head = {s.rowsDropped() + 4, 79};
    sel.layoutEpoch = s.layoutEpoch();
    sel.active = true;
    EXPECT_EQ(sel.text(s), "line 3\nline 4");

    // Reflow and growing the screen bring history rows back at full width.
    t.resize(40, 4, 8, 16);
    EXPECT_EQ(rowText(t.screen(), t.screen().absoluteRow(10)), "line 10");
    t.resize(40, 20, 8, 16);
    for (int y = 0; y < t.rows(); ++y) {
        EXPECT_EQ(t.screen().row(y).cells.size(), 40u);
    }
}

TEST(Reflow, TrackedWideTailKeepsItsCellOffset)
{
    Term t(10, 3);
    t.t.feed("界abc");
    std::vector<std::size_t> rows{0, 0};
    std::vector<int> columns{1, 2};
    t.t.screen().resize(6, 3, true, std::nullopt, &rows, &columns);
    EXPECT_EQ(rows, (std::vector<std::size_t>{0, 0}));
    EXPECT_EQ(columns, (std::vector<int>{1, 2}));
}

TEST(Reflow, FittingHistoryRetainsCellsStylesAndTrackedPositions)
{
    Term t(20, 3);
    t.t.feed("\x1b[31mA\xcc\x81\xe7\x95\x8c\x1b[0m\r\nsecond\r\nthird\r\nprompt");
    auto& s = t.t.screen();
    ASSERT_EQ(s.historySize(), 1u);
    const Cell* cells = s.absoluteRow(0).cells.data();
    const auto style = s.absoluteRow(0).at(0).styleId();
    std::vector<std::size_t> tracked{0, 0, 0};
    std::vector<int> columns{0, 2, 15};
    s.resize(10, 3, true, std::nullopt, &tracked, &columns);
    EXPECT_EQ(s.absoluteRow(0).cells.data(), cells);
    EXPECT_EQ(tracked, (std::vector<std::size_t>{0, 0, 0}));
    EXPECT_EQ(columns, (std::vector<int>{0, 2, 10}));
    EXPECT_EQ(s.absoluteRow(0).at(0).styleId(), style);
    EXPECT_EQ(s.styles().get(style).fg, Color::palette(1));
    EXPECT_EQ(s.graphemeExtra(s.absoluteRow(0), 0), U"\u0301");
    EXPECT_EQ(rowText(s, s.absoluteRow(0)), "A\xcc\x81\xe7\x95\x8c");

    // Growing height pulls the retained row back into the writable grid.
    s.resize(12, 6, true);
    for (int y = 0; y < s.rows(); ++y) EXPECT_EQ(s.row(y).cells.size(), 12u);
    EXPECT_EQ(rowText(s, s.row(0)), "A\xcc\x81\xe7\x95\x8c");
    s.writeCell(s.row(0), 11, U'!', StyleTable::DefaultId, 1);
    EXPECT_EQ(s.row(0).at(11).codepoint(), U'!');
}

TEST(Reflow, ShortContinuationStillJoinsWrappedHistory)
{
    Term t(6, 2);
    t.t.feed("abcdefgh\r\nnext\r\nprompt");
    ASSERT_EQ(t.t.screen().historySize(), 2u);
    t.t.resize(12, 2, 8, 16);
    const auto& s = t.t.screen();
    EXPECT_EQ(s.historySize(), 1u);
    EXPECT_EQ(rowText(s, s.absoluteRow(0)), "abcdefgh");
    EXPECT_FALSE(s.absoluteRow(0).wrapped);
    EXPECT_EQ(rowText(t.t, 0), "next");
    EXPECT_EQ(rowText(t.t, 1), "prompt");
}

namespace {
// Compares two screens cell by cell. Style ids and link ids are table
// indexes, so the comparison resolves them.
void expectSameGrid(const Screen& a, const Screen& b, const std::string& context)
{
    SCOPED_TRACE(context);
    ASSERT_EQ(a.cols(), b.cols());
    ASSERT_EQ(a.rows(), b.rows());
    ASSERT_EQ(a.historySize(), b.historySize());
    EXPECT_EQ(a.cursor.x, b.cursor.x);
    EXPECT_EQ(a.cursor.y, b.cursor.y);
    for (std::size_t i = 0; i < a.historySize() + std::size_t(a.rows()); ++i) {
        const Row& ra = a.absoluteRow(i);
        const Row& rb = b.absoluteRow(i);
        ASSERT_EQ(ra.wrapped, rb.wrapped) << "row " << i;
        for (int x = 0; x < a.cols(); ++x) {
            const Cell& ca = ra.at(x);
            const Cell& cb = rb.at(x);
            ASSERT_EQ(ca.codepoint(), cb.codepoint()) << "row " << i << " col " << x;
            ASSERT_EQ(ca.width(), cb.width()) << "row " << i << " col " << x;
            ASSERT_EQ(ca.isSpacerHead(), cb.isSpacerHead()) << "row " << i << " col " << x;
            ASSERT_TRUE(a.graphemeExtra(ra, x) == b.graphemeExtra(rb, x)) << "row " << i << " col " << x;
            Style sa = a.styles().get(ca.styleId());
            Style sb = b.styles().get(cb.styleId());
            ASSERT_EQ(a.styles().linkUri(sa.link), b.styles().linkUri(sb.link)) << "row " << i << " col " << x;
            sa.link = sb.link = 0;
            ASSERT_TRUE(sa == sb) << "row " << i << " col " << x;
        }
    }
    EXPECT_EQ(a.styles().liveCount(), b.styles().liveCount());
    EXPECT_EQ(a.styles().liveLinks(), b.styles().liveLinks());
}

// A line of about @p cells cells with styled runs, hyperlinks, wide
// characters and combining marks. It ends with the pen reset. Without
// @p background, runs are underlined instead: a pen background fills the
// rows that output scrolls in, and those fills depend on the width.
std::string mixedLine(std::mt19937& rng, int cells, bool background)
{
    static const char* const Wide[] = {"\xe7\x95\x8c", "\xe6\x97\xa5", "\xed\x95\x9c", "\xf0\x9f\x98\x80"};
    std::string out;
    for (int n = 0; n < cells;) {
        const unsigned pick = rng() % 100;
        if (pick < 10) {
            out += Wide[rng() % 4];
            n += 2;
        } else if (pick < 15) {
            out += "e\xcc\x81"; // e + combining acute accent
            ++n;
        } else if (pick < 19) {
            out += "\x1b[3" + std::to_string(rng() % 8) + (background ? ";4" + std::to_string(rng() % 8) : ";4") + "m";
        } else if (pick < 21) {
            out += "\x1b[1;3m";
        } else if (pick < 23) {
            out += "\x1b[0m";
        } else if (pick < 24) {
            out += "\x1b]8;;https://example.org/" + std::to_string(rng() % 3) + "\x1b\\";
        } else if (pick < 25) {
            out += "\x1b]8;;\x1b\\";
        } else {
            out.push_back(rng() % 6 == 0 ? ' ' : char('a' + rng() % 26));
            ++n;
        }
    }
    return out + "\x1b[0m\x1b]8;;\x1b\\";
}
} // namespace

TEST(Reflow, LongWrappedLinesMatchOutputWrittenAtTheNewWidth)
{
    // Reflow must give what the same output gives when written at the new
    // width: rewrapping keeps text, styles, links, graphemes, wide
    // characters, the cursor and the history row count.
    for (unsigned seed = 1; seed <= 40; ++seed) {
        std::mt19937 rng{seed};
        std::string data;
        const int lines = 4 + int(rng() % 12);
        for (int l = 0; l < lines; ++l) {
            const int cells = rng() % 3 == 0 ? int(rng() % 40) : 80 + int(rng() % 900);
            data += mixedLine(rng, cells, false) + "\r\n";
        }
        // A short last line: a cursor at a pending wrap is not part of this check.
        data += mixedLine(rng, int(rng() % 200), false) + "\r\n$ ";
        const int rows = 3 + int(rng() % 6);
        RecordingHost host;
        Terminal t(10 + int(rng() % 90), rows, host);
        t.feed(data);
        for (int step = 0; step < 4; ++step) {
            const int cols = 3 + int(rng() % 120);
            t.resize(cols, rows, 8, 16);
            RecordingHost freshHost;
            Terminal fresh(cols, rows, freshHost);
            fresh.feed(data);
            expectSameGrid(t.screen(), fresh.screen(),
                           "seed " + std::to_string(seed) + " step " + std::to_string(step) + " cols "
                               + std::to_string(cols));
            if (testing::Test::HasFatalFailure()) {
                return;
            }
        }
    }
}

TEST(Reflow, LongWrappedLineRoundTripRestoresEveryCell)
{
    std::mt19937 rng{7};
    std::string data;
    for (int l = 0; l < 6; ++l) {
        data += mixedLine(rng, 300 + l * 211, true) + "\r\n";
    }
    RecordingHost host;
    Terminal t(50, 6, host);
    t.feed(data + "$ ");
    RecordingHost originalHost;
    Terminal original(50, 6, originalHost);
    original.feed(data + "$ ");
    for (const int cols : {13, 97, 3, 49, 211, 50}) {
        t.resize(cols, 6, 8, 16);
    }
    expectSameGrid(t.screen(), original.screen(), "after round trip");
}

TEST(Reflow, WideCharNeverSplitsAcrossWrappedRows)
{
    Term t(20, 3);
    std::string line = "a";
    for (int i = 0; i < 40; ++i) {
        line += "\xe7\x95\x8c"; // a wide character
    }
    t.t.feed(line + "\r\n$ ");
    for (const int cols : {9, 10, 7, 20}) {
        t.t.resize(cols, 3, 8, 16);
        const Screen& s = t.t.screen();
        std::size_t wide = 0;
        for (std::size_t i = 0; i < s.historySize() + std::size_t(s.rows()); ++i) {
            const Row& r = s.absoluteRow(i);
            for (int x = 0; x < cols; ++x) {
                if (r.at(x).width() == 2) {
                    ++wide;
                    ASSERT_LT(x + 1, cols) << "a wide head in the last column, cols " << cols;
                    EXPECT_TRUE(r.at(x + 1).isTail());
                }
            }
            // A wide character that does not fit leaves a spacer head.
            if (r.wrapped) {
                const Cell& last = r.at(cols - 1);
                EXPECT_TRUE(last.isTail() || last.isSpacerHead()) << "row " << i << ", cols " << cols;
            }
        }
        EXPECT_EQ(wide, 40u) << "cols " << cols;
    }
}

TEST(Reflow, StyleAndLinkRunAcrossWrapsKeepItsReferences)
{
    Term t(10, 3);
    t.t.feed("\x1b[31;1m\x1b]8;;https://example.org/x\x1b\\");
    t.t.feed(std::string(37, 'x'));
    t.t.feed("\x1b]8;;\x1b\\\x1b[0m\r\n$ ");
    const auto styles = t.t.screen().styles().liveCount();
    const auto links = t.t.screen().styles().liveLinks();
    for (const int cols : {7, 23, 3, 41, 10}) {
        t.t.resize(cols, 3, 8, 16);
        const Screen& s = t.t.screen();
        int seen = 0;
        for (std::size_t i = 0; i < s.historySize() + std::size_t(s.rows()); ++i) {
            const Row& r = s.absoluteRow(i);
            for (int x = 0; x < cols; ++x) {
                if (r.at(x).codepoint() != U'x') {
                    continue;
                }
                ++seen;
                const Style& style = s.styles().get(r.at(x).styleId());
                EXPECT_EQ(style.fg, Color::palette(1));
                EXPECT_TRUE(style.has(Attr::Bold));
                EXPECT_EQ(s.styles().linkUri(style.link), "https://example.org/x");
            }
        }
        EXPECT_EQ(seen, 37) << "cols " << cols;
        EXPECT_EQ(s.styles().liveCount(), styles) << "cols " << cols;
        EXPECT_EQ(s.styles().liveLinks(), links) << "cols " << cols;
    }
}

TEST(Reflow, CursorAndTrackedPositionsFollowLongWrappedLine)
{
    // Every character of the long line is different, so a position is
    // checked by the character at it.
    std::string line;
    for (int i = 0; i < 300; ++i) {
        appendUtf8(line, i % 25 == 7 ? char32_t(0x4E00 + i) : char32_t(0x100 + i)); // some wide
    }
    Term t(40, 4);
    t.t.feed(line + "\r\n" + line + "\x1b[23D"); // the cursor moves back into the second line
    Screen& s = t.t.screen();
    const char32_t atCursor = s.row(s.cursor.y).at(s.cursor.x).codepoint();
    ASSERT_NE(atCursor, 0u);
    // Track cells in the wrapped rows of the first line.
    std::vector<std::size_t> rows;
    std::vector<int> columns;
    std::vector<char32_t> expected;
    for (const auto& [row, col] : {std::pair{1, 3}, std::pair{4, 0}, std::pair{6, 17}, std::pair{7, 38}}) {
        const Cell& c = s.absoluteRow(std::size_t(row)).at(col);
        if (c.isTail() || c.isSpacerHead() || c.isEmpty()) {
            continue;
        }
        rows.push_back(std::size_t(row));
        columns.push_back(col);
        expected.push_back(c.codepoint());
    }
    ASSERT_GE(expected.size(), 3u);
    for (const int cols : {17, 61, 9, 40}) {
        s.resize(cols, 4, true, std::nullopt, &rows, &columns);
        for (std::size_t k = 0; k < expected.size(); ++k) {
            ASSERT_NE(rows[k], Screen::TrackedRowGone);
            EXPECT_EQ(s.absoluteRow(rows[k]).at(columns[k]).codepoint(), expected[k]) << "cols " << cols;
        }
        EXPECT_EQ(s.row(s.cursor.y).at(s.cursor.x).codepoint(), atCursor) << "cols " << cols;
    }
}

TEST(Edit, InsertAtAWideCharacterShiftsItWhole)
{
    Term ich(10, 2);
    ich.t.feed("\xe4\xb8\xad\x1b[1;1H\x1b[@"); // ICH before 中
    EXPECT_EQ(rowText(ich.t, 0), " \xe4\xb8\xad");
    Term irm(10, 2);
    irm.t.feed("\xe4\xb8\xad\x1b[1;1H\x1b[4ha"); // insert mode
    EXPECT_EQ(rowText(irm.t, 0), "a\xe4\xb8\xad");
}

TEST(Edit, DeleteUpToAWideCharacterShiftsItWhole)
{
    Term dch(10, 2);
    dch.t.feed("ab\xe4\xb8\xad\x1b[1;1H\x1b[2P");
    EXPECT_EQ(rowText(dch.t, 0), "\xe4\xb8\xad");
    // Inside DECSLRM margins (columns 1-4), a wide character beyond the
    // right margin is not touched.
    Term margins(10, 2);
    margins.t.feed("\x1b[?69h\x1b[1;4s\x1b[1;5H\xe4\xb8\xad\x1b[1;1H\x1b[P");
    EXPECT_EQ(margins.t.screen().row(0).cells[4].width(), 2);
    EXPECT_EQ(rowText(margins.t, 0), "    \xe4\xb8\xad");
}

TEST(Edit, SpacerHeadStaysInTheLastColumn)
{
    Term t(10, 3);
    t.t.feed("abcdefghi\xe4\xb8\xad"); // 中 wraps; column 9 keeps a spacer head
    ASSERT_TRUE(t.t.screen().row(0).cells[9].isSpacerHead());
    t.t.feed("\x1b[1;1H\x1b[P\x1b[1;10HX");
    EXPECT_FALSE(t.t.screen().row(0).cells[8].isSpacerHead());
    const Screen& s = t.t.screen();
    Selection sel;
    sel.anchor = {s.rowsDropped(), 0};
    sel.head = {s.rowsDropped() + 1, 9};
    sel.layoutEpoch = s.layoutEpoch();
    sel.active = true;
    EXPECT_EQ(sel.text(s), "bcdefghi X\xe4\xb8\xad"); // the moved blank is text now

    Term alt(10, 3);
    alt.t.feed("\x1b[?1049habcdefghi\xe4\xb8\xad");
    ASSERT_TRUE(alt.t.screen().row(0).cells[9].isSpacerHead());
    alt.t.resize(12, 3, 8, 16); // no reflow: the old last column is a blank now
    EXPECT_FALSE(alt.t.screen().row(0).cells[9].isSpacerHead());
}

TEST(TextSizing, ScrollWithoutHistoryKeepsOtherBlocks)
{
    Term t(10, 5);
    t.t.feed("\x1b[?1049h\x1b[1;1H\x1b]66;s=2;A\x1b\\"); // A on rows 0-1
    t.t.feed("\x1b[5;1H\n"); // A's top leaves; its lower row is row 0
    t.t.feed("\x1b]66;w=2;B\x1b\\"); // B on row 4
    t.t.feed("\n");
    const Screen& s = t.t.screen();
    EXPECT_TRUE(s.row(3).cells[0].isMulticell());
    EXPECT_NE(s.multicellParams(s.row(3), 0), nullptr);
}

TEST(TextSizing, ScrollInsideMarginsKeepsBlocksOutsideThem)
{
    Term t(80, 24);
    t.t.feed("\x1b[5;10r\x1b[5;1H\x1b]66;s=2;A\x1b\\"); // A on rows 4-5, in the region
    t.t.feed("\x1b[4;1H\x1b]66;w=2;B\x1b\\"); // B on row 3, above it
    t.t.feed("\x1b[11;1H\x1b]66;s=2;C\x1b\\"); // C on rows 10-11, below it
    t.t.feed("\x1b[5;1H\x1b[S\x1b[5;1H\x1b[2K"); // the scroll splits A
    const Screen& s = t.t.screen();
    EXPECT_TRUE(s.row(3).cells[0].isMulticell());
    EXPECT_NE(s.multicellParams(s.row(3), 0), nullptr);
    EXPECT_NE(s.multicellParams(s.row(10), 0), nullptr);
    EXPECT_NE(s.multicellParams(s.row(11), 0), nullptr);
    for (int y = 4; y <= 9; ++y) {
        EXPECT_FALSE(s.row(y).cells[0].isMulticell()) << y; // no part of A is left behind
    }
}

TEST(TextSizing, BlockTextIsBoundedByItsRow)
{
    // A row holds no more grapheme text than its cells could one by one
    // (10 x 32 here), and each row of a block keeps the same copy.
    Term t(10, 3);
    std::string text = "\x1b]66;s=2:w=1;e";
    for (int i = 0; i < 1000; ++i) {
        text += "\xcc\x81"; // combining acute accent
    }
    t.t.feed(text + "\x1b\\");
    const Screen& s = t.t.screen();
    ASSERT_TRUE(s.row(0).cells[0].isMulticell());
    EXPECT_EQ(s.graphemeExtra(s.row(0), 0).size(), 320u);
    EXPECT_EQ(s.graphemeExtra(s.row(1), 0), s.graphemeExtra(s.row(0), 0));
    // A second block in the full row still gets what one cell may hold.
    t.t.feed("\x1b]66;w=1;" + std::string(100, 'x') + "\x1b\\");
    EXPECT_EQ(s.graphemeExtra(s.row(0), 2).size(), 32u);
}

TEST(Resize, HeightShrinkKeepsTheCursorOnItsLine)
{
    Term t(20, 24);
    t.t.feed("\x1b[?1049h");
    for (int y = 0; y < 24; ++y) {
        t.t.feed("\x1b[" + std::to_string(y + 1) + ";1Hr" + std::to_string(y));
    }
    t.t.feed("\x1b[3;1H");
    t.t.resize(20, 10, 8, 16);
    EXPECT_EQ(t.t.screen().cursor.y, 2);
    EXPECT_EQ(rowText(t.t, 2), "r2");
    EXPECT_EQ(rowText(t.t, 9), "r9"); // rows below the screen were dropped
}

TEST(Resize, SavedCursorFollowsItsLineThroughReflow)
{
    Term t(40, 5);
    t.t.feed(std::string(30, 'A') + "\r\n"); // the cursor is on row 1
    t.t.feed("\x1b[?1049h");
    t.t.resize(20, 5, 8, 16); // the A line takes rows 0-1 of the primary screen
    t.t.feed("\x1b[?1049l$ ");
    EXPECT_EQ(rowText(t.t, 1), std::string(10, 'A'));
    EXPECT_EQ(rowText(t.t, 2), "$");
}

TEST(Resize, PendingWrapSurvives)
{
    Term high(80, 3);
    high.t.feed(std::string(80, 'a'));
    high.t.resize(80, 5, 8, 16); // height only
    high.t.feed("b");
    EXPECT_EQ(rowText(high.t, 0), std::string(80, 'a'));
    EXPECT_EQ(rowText(high.t, 1), "b");

    Term wide(80, 3);
    wide.t.feed(std::string(80, 'a'));
    wide.t.resize(100, 3, 8, 16); // the line has room after its last character
    wide.t.feed("b");
    EXPECT_EQ(rowText(wide.t, 0), std::string(80, 'a') + "b");

    Term narrow(80, 3);
    narrow.t.feed(std::string(80, 'a'));
    narrow.t.resize(40, 3, 8, 16); // two full rows: the wrap is pending again
    narrow.t.feed("b");
    EXPECT_EQ(rowText(narrow.t, 1), std::string(40, 'a'));
    EXPECT_EQ(rowText(narrow.t, 2), "b");
}

TEST(Resize, ScrolledBackViewKeepsItsPlaceThroughReflow)
{
    Term t(10, 3);
    for (int i = 0; i < 20; ++i) {
        t.t.feed("r" + std::to_string(i) + "\r\n");
    }
    t.t.scrollViewport(5);
    const Screen& before = t.t.screen();
    const std::string top = rowText(before, before.absoluteRow(before.viewportTop()));
    ASSERT_FALSE(top.empty());
    t.t.resize(6, 3, 8, 16);
    const Screen& s = t.t.screen();
    EXPECT_GT(s.viewportOffset(), 0u);
    EXPECT_EQ(rowText(s, s.absoluteRow(s.viewportTop())), top);
}

TEST(Resize, LineSizeSurvivesReflowWhenTheLineFits)
{
    Term t(20, 4);
    t.t.feed("\x1b#6HELLO\r\n\x1b#3BIG\r\n\x1b#4BIG\r\n\x1b#6ABCDEFGHI");
    t.t.resize(16, 4, 8, 16);
    const Screen& s = t.t.screen();
    EXPECT_EQ(s.row(0).lineSize, LineSize::DoubleWidth);
    EXPECT_EQ(s.row(1).lineSize, LineSize::DoubleTop);
    EXPECT_EQ(s.row(2).lineSize, LineSize::DoubleBottom);
    EXPECT_EQ(s.row(3).lineSize, LineSize::Single); // 9 cells do not fit in 8 double-width ones
}

TEST(Resize, PrimaryScreenReflowsWithoutScrollback)
{
    RecordingHost host;
    TerminalOptions options;
    options.maxScrollback = 0;
    Terminal t(10, 3, host, options);
    t.feed("0123456789AB");
    t.resize(5, 3, 8, 16);
    EXPECT_EQ(screenText(t), "01234\n56789\nAB");
}

TEST(Resize, HeightChangeKeepsTheLayoutEpoch)
{
    Term t(10, 3);
    const std::uint64_t epoch = t.t.screen().layoutEpoch();
    t.t.resize(10, 6, 8, 16);
    EXPECT_EQ(t.t.screen().layoutEpoch(), epoch);
    t.t.resize(12, 6, 8, 16);
    EXPECT_NE(t.t.screen().layoutEpoch(), epoch);
}
