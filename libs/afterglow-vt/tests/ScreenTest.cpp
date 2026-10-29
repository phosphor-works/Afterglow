// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

#include <gtest/gtest.h>

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
    t.t.resize(20, 8, 8, 16); // height only: nothing blanked
    EXPECT_FALSE(t.t.promptRedrawPending());
    t.t.resize(15, 8, 8, 16);
    EXPECT_TRUE(t.t.promptRedrawPending());
    t.t.feed("\r\x1b]133;A\x1b\\\x1b[J> ");
    EXPECT_TRUE(t.t.promptRedrawPending());
    t.t.feed("\x1b]133;B\x1b\\");
    EXPECT_FALSE(t.t.promptRedrawPending());
}
