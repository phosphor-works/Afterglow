// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The VT features esctest exercises, checked without the suite: reports,
// rectangular and column editing, protection, reverse wrap, modes.

#include "TestUtil.h"

#include <gtest/gtest.h>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {

struct ResizingHost : RecordingHost
{
    bool requestResize(int, int) override { return true; }
};

template<typename Host = RecordingHost>
struct Term
{
    Host host;
    Terminal t;
    Term(int cols, int rows)
        : t(cols, rows, host)
    {
        t.resize(cols, rows, 10, 20);
    }
    void feed(std::string_view s) { t.feed(s); }
    std::string take()
    {
        std::string out;
        out.swap(host.written);
        return out;
    }
};

} // namespace

TEST(Conformance, RectChecksum)
{
    Term t(10, 4);
    t.feed("ab");
    // DECRQCRA: the sum of the characters, blanks counting as spaces.
    t.feed("\x1b[7;1;1;1;1;2*y");
    EXPECT_EQ(t.take(), "\x1bP7!~00C3\x1b\\"); // 'a' + 'b'
    t.feed("\x1b[8;1;1;2;1;3*y");
    EXPECT_EQ(t.take(), "\x1bP8!~0082\x1b\\"); // 'b' + ' '
}

TEST(Conformance, FillEraseAndCopyRectangles)
{
    Term t(8, 4);
    t.feed("\x1b[42;2;2;3;4$x"); // DECFRA '*' over rows 2-3, columns 2-4
    EXPECT_EQ(rowText(t.t, 1), " ***");
    EXPECT_EQ(rowText(t.t, 2), " ***");
    t.feed("\x1b[2;3;3;3$z"); // DECERA one column
    EXPECT_EQ(rowText(t.t, 1), " * *");
    t.feed("\x1b[2;2;3;4;1;2;5;1$v"); // DECCRA to row 2, column 5
    EXPECT_EQ(rowText(t.t, 1), " * ** *");
    EXPECT_EQ(rowText(t.t, 2), " * ** *");
}

TEST(Conformance, CopyRectangleOverlapping)
{
    Term t(8, 2);
    t.feed("abcdef");
    t.feed("\x1b[1;1;1;4;1;1;3;1$v"); // copy "abcd" two columns right
    EXPECT_EQ(rowText(t.t, 0), "ababcd");
}

TEST(Conformance, InsertAndDeleteColumns)
{
    Term t(6, 3);
    t.feed("abcdef\r\nghijkl\x1b[1;3H");
    t.feed("\x1b['}"); // DECIC
    EXPECT_EQ(rowText(t.t, 0), "ab cde");
    EXPECT_EQ(rowText(t.t, 1), "gh ijk");
    t.feed("\x1b[2'~"); // DECDC
    EXPECT_EQ(rowText(t.t, 0), "abde");
    EXPECT_EQ(rowText(t.t, 1), "ghjk");
}

TEST(Conformance, BackAndForwardIndex)
{
    Term t(4, 1);
    t.feed("abcd\x1b[1;1H\x1b" "6"); // DECBI at the left margin scrolls right
    EXPECT_EQ(rowText(t.t, 0), " abc");
    t.feed("\x1b[1;4H\x1b" "9"); // DECFI at the right margin scrolls left
    EXPECT_EQ(rowText(t.t, 0), "abc");
}

TEST(Conformance, DecProtectionOnlyStopsSelectiveErase)
{
    Term t(6, 2);
    t.feed("a\x1b[1\"qb\x1b[0\"qc");
    t.feed("\x1b[?2K"); // DECSEL spares b
    EXPECT_EQ(rowText(t.t, 0), " b");
    t.feed("\x1b[2K"); // EL does not
    EXPECT_EQ(rowText(t.t, 0), "");
}

TEST(Conformance, IsoProtectionOnlyStopsOrdinaryErase)
{
    Term t(6, 2);
    t.feed("a\x1bVb\x1bWc\x1b[1;1H");
    t.feed("\x1b[K"); // EL spares b
    EXPECT_EQ(rowText(t.t, 0), " b");
    t.feed("\x1b[?K"); // DECSEL does not
    EXPECT_EQ(rowText(t.t, 0), "");
}

TEST(Conformance, SgrResetKeepsProtection)
{
    Term t(6, 1);
    t.feed("\x1b[1\"q\x1b[0ma\x1b[0\"q\x1b[1;1H\x1b[?K");
    EXPECT_EQ(rowText(t.t, 0), "a");
}

TEST(Conformance, ReverseWrapFollowsSoftWraps)
{
    Term t(4, 4);
    t.feed("\x1b[?45h");
    t.feed("abcdef"); // wraps onto row 2
    t.feed("\b\b\b"); // f, e, then into the wrapped row above
    EXPECT_EQ(t.t.screen().cursor.y, 0);
    EXPECT_EQ(t.t.screen().cursor.x, 3);
    t.feed("\x1b[3;1H\b"); // row 2 did not wrap into row 3: stays
    EXPECT_EQ(t.t.screen().cursor.y, 2);
    EXPECT_EQ(t.t.screen().cursor.x, 0);
}

TEST(Conformance, ExtendedReverseWrapCrossesLinesAndTopMargin)
{
    Term t(4, 4);
    t.feed("\x1b[?1045h\x1b[3;1H\b");
    EXPECT_EQ(t.t.screen().cursor.y, 1);
    EXPECT_EQ(t.t.screen().cursor.x, 3);
    t.feed("\x1b[1;1H\b"); // top-left wraps to the bottom-right
    EXPECT_EQ(t.t.screen().cursor.y, 3);
    EXPECT_EQ(t.t.screen().cursor.x, 3);
    t.feed("\x1b[!p\x1b[1;1H\b"); // DECSTR turns it off
    EXPECT_EQ(t.t.screen().cursor.y, 0);
}

TEST(Conformance, BackspaceFromPendingWrap)
{
    Term t(4, 2);
    t.feed("abcd\bX"); // leaves the pending wrap and moves left
    EXPECT_EQ(rowText(t.t, 0), "abXd");
}

TEST(Conformance, TabInPendingWrapKeepsTheWrap)
{
    Term t(4, 2);
    t.feed("abcd\tX");
    EXPECT_EQ(rowText(t.t, 0), "abcd");
    EXPECT_EQ(rowText(t.t, 1), "X");
}

TEST(Conformance, ModeReports)
{
    Term t(10, 2);
    t.feed("\x1b[?45h\x1b[?45$p\x1b[?1045$p\x1b[2$p\x1b[1$p");
    EXPECT_EQ(t.take(), "\x1b[?45;1$y\x1b[?1045;2$y\x1b[2;2$y\x1b[1;4$y");
    // Below level 3 there is no DECRQM; DECSCL resets to the level asked.
    t.feed("\x1b[62;1\"p\x1b[?45$p");
    EXPECT_EQ(t.take(), "");
}

TEST(Conformance, SaveAndRestoreModes)
{
    Term t(10, 2);
    t.feed("\x1b[?7l\x1b[?7s\x1b[?7h\x1b[?7r\x1b[?7$p");
    EXPECT_EQ(t.take(), "\x1b[?7;2$y");
}

TEST(Conformance, ColumnModeNeedsPermissionAndClears)
{
    Term<ResizingHost> t(80, 4);
    t.feed("x\x1b[?3h");
    EXPECT_EQ(t.t.cols(), 80); // mode 40 not set
    t.feed("\x1b[?40h\x1b[?3h");
    EXPECT_EQ(t.t.cols(), 132);
    EXPECT_EQ(rowText(t.t, 0), "");
    t.feed("\x1b" "c"); // RIS returns to 80
    EXPECT_EQ(t.t.cols(), 80);
}

TEST(Conformance, WindowResizeTellsOmittedFromZero)
{
    struct Host : ResizingHost
    {
        bool screenSize(int& w, int& h) override
        {
            w = 1000;
            h = 400;
            return true;
        }
    };
    Term<Host> t(20, 5);
    t.feed("\x1b[8;;30t"); // omitted height keeps it
    EXPECT_EQ(t.t.rows(), 5);
    EXPECT_EQ(t.t.cols(), 30);
    t.feed("\x1b[8;0;30t"); // zero means the display: 400 / 20
    EXPECT_EQ(t.t.rows(), 20);
}

TEST(Conformance, ColorSpecsFollowXParseColor)
{
    Term t(10, 2);
    t.feed("\x1b]4;1;#fff\x1b\\\x1b]4;1;?\x1b\\");
    EXPECT_EQ(t.take(), "\x1b]4;1;rgb:f0f0/f0f0/f0f0\x1b\\");
    t.feed("\x1b]4;1;rgbi:1/0/0.5\x1b\\\x1b]4;1;?\x1b\\");
    EXPECT_EQ(t.take(), "\x1b]4;1;rgb:ffff/0000/8080\x1b\\");
}

TEST(Conformance, CharacterSetsAndSingleShifts)
{
    Term t(20, 2);
    t.feed("\x1b(A#\x1b(B#"); // British, then ASCII
    EXPECT_EQ(rowText(t.t, 0), "£#");
    t.feed("\r\n\x1b.A\x1bNAB\x1bOC"); // Latin-1 in G2, SS2 for one character; G3 stays ASCII
    EXPECT_EQ(rowText(t.t, 1), "ÁBC");
}

TEST(Conformance, TertiaryAttributesAndTerminalParameters)
{
    Term t(10, 2);
    t.feed("\x1b[=c\x1b[x\x1b[1x");
    EXPECT_EQ(t.take(), "\x1bP!|00000000\x1b\\\x1b[2;1;1;128;128;1;0x\x1b[3;1;1;128;128;1;0x");
}

TEST(Conformance, Vt52Mode)
{
    Term t(20, 5);
    t.feed("\x1b[?2l"); // into VT52 mode
    t.feed("\x1bY\"%ab\x1b" "D\x1b" "DX"); // row 2, column 5; back twice
    EXPECT_EQ(rowText(t.t, 2), "     Xb");
    t.feed("\x1bY~#"); // row off the screen: only the column moves
    EXPECT_EQ(t.t.screen().cursor.y, 2);
    EXPECT_EQ(t.t.screen().cursor.x, 3);
    t.feed("\x1bZ");
    EXPECT_EQ(t.take(), "\x1b/Z");
    t.feed("\x1bH\x1b" "Fa\x1bGa"); // graphics character set on and off
    EXPECT_EQ(rowText(t.t, 0), "\u2592a");
    t.feed("\x1b<\x1b[?2$p"); // back to ANSI; DECRQM works again
    EXPECT_EQ(t.take(), "\x1b[?2;1$y");
}

TEST(Conformance, ColorStack)
{
    Term t(10, 2);
    t.feed("\x1b]4;1;rgb:11/22/33\x1b\\\x1b]10;rgb:aa/bb/cc\x1b\\");
    t.feed("\x1b[#P"); // XTPUSHCOLORS
    t.feed("\x1b]4;1;rgb:44/55/66\x1b\\\x1b]10;rgb:00/00/00\x1b\\");
    t.feed("\x1b[#R");
    EXPECT_EQ(t.take(), "\x1b[1;1#Q");
    t.feed("\x1b[#Q\x1b]4;1;?\x1b\\\x1b]10;?\x1b\\"); // XTPOPCOLORS restores both
    EXPECT_EQ(t.take(), "\x1b]4;1;rgb:1111/2222/3333\x1b\\\x1b]10;rgb:aaaa/bbbb/cccc\x1b\\");
    t.feed("\x1b]30001\x1b\\\x1b]4;1;rgb:99/99/99\x1b\\\x1b]30101\x1b\\\x1b]4;1;?\x1b\\"); // kitty's push/pop
    EXPECT_EQ(t.take(), "\x1b]4;1;rgb:1111/2222/3333\x1b\\");
}

TEST(Conformance, KittyColorControl)
{
    Term t(10, 2);
    t.feed("\x1b]21;foreground=#102030;7=rgb:ff/00/00\x1b\\");
    t.feed("\x1b]21;foreground=?;7=?;cursor=?;selection_background=?;bogus=?\x1b\\");
    EXPECT_EQ(t.take(), "\x1b]21;foreground=rgb:10/20/30;7=rgb:ff/00/00;cursor=;selection_background=;unknown=Ym9ndXM\x1b\\");
    t.feed("\x1b]21;foreground;cursor=#abcdef\x07\x1b]21;cursor=?\x07"); // bare key resets
    EXPECT_EQ(t.take(), "\x1b]21;cursor=rgb:ab/cd/ef\x07");
    EXPECT_EQ(t.t.defaultForeground(), TerminalOptions{}.defaultForeground);
}
