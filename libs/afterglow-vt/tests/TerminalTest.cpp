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
    Term(int cols, int rows, std::size_t scrollback = 100)
        : t(cols, rows, host, [scrollback] {
            TerminalOptions o;
            o.maxScrollback = scrollback;
            return o;
        }())
    {
    }
    void feed(std::string_view s) { t.feed(s); }
};
} // namespace

TEST(Terminal, PrintAndWrap)
{
    Term t(5, 3);
    t.feed("abcdefg");
    EXPECT_EQ(rowText(t.t, 0), "abcde");
    EXPECT_EQ(rowText(t.t, 1), "fg");
    EXPECT_TRUE(t.t.screen().row(0).wrapped);
    EXPECT_EQ(t.t.screen().cursor.x, 2);
}

TEST(Terminal, PendingWrapThenCarriageReturn)
{
    Term t(5, 3);
    t.feed("abcde\rX");
    EXPECT_EQ(rowText(t.t, 0), "Xbcde");
    EXPECT_EQ(rowText(t.t, 1), "");
}

TEST(Terminal, NoAutowrapOverwritesLastColumn)
{
    Term t(5, 3);
    t.feed("\x1b[?7labcdefg");
    EXPECT_EQ(rowText(t.t, 0), "abcdg");
}

TEST(Terminal, CursorMovementAndErase)
{
    Term t(10, 4);
    t.feed("0123456789\x1b[2;3Hxy\x1b[1;5H\x1b[K");
    EXPECT_EQ(rowText(t.t, 0), "0123");
    EXPECT_EQ(rowText(t.t, 1), "  xy");
    t.feed("\x1b[2J");
    EXPECT_EQ(screenText(t.t), "");
}

TEST(Terminal, ScrollingFeedsHistory)
{
    Term t(5, 2);
    t.feed("a\r\nb\r\nc\r\nd");
    EXPECT_EQ(rowText(t.t, 0), "c");
    EXPECT_EQ(rowText(t.t, 1), "d");
    ASSERT_EQ(t.t.screen().historySize(), 2u);
    EXPECT_EQ(rowText(t.t.screen(), t.t.screen().absoluteRow(0)), "a");
}

TEST(Terminal, ScrollRegion)
{
    Term t(5, 4);
    t.feed("1\r\n2\r\n3\r\n4\x1b[2;3r\x1b[3;1H\nX");
    EXPECT_EQ(screenText(t.t), "1\n3\nX\n4");
    EXPECT_EQ(t.t.screen().historySize(), 0u);
}

TEST(Terminal, InsertDeleteLines)
{
    Term t(5, 4);
    t.feed("1\r\n2\r\n3\r\n4\x1b[2;1H\x1b[L");
    EXPECT_EQ(screenText(t.t), "1\n\n2\n3");
    t.feed("\x1b[M\x1b[M");
    EXPECT_EQ(screenText(t.t), "1\n3");
    EXPECT_EQ(t.t.screen().historySize(), 0u);
}

TEST(Terminal, InsertDeleteChars)
{
    Term t(8, 2);
    t.feed("abcdef\x1b[1;3H\x1b[2@");
    EXPECT_EQ(rowText(t.t, 0), "ab  cdef");
    t.feed("\x1b[3P");
    EXPECT_EQ(rowText(t.t, 0), "abdef");
}

TEST(Terminal, SgrColorsAndAttributes)
{
    Term t(10, 2);
    t.feed("\x1b[1;3;38;5;196;48:2::1:2:3;4:3;58;2;9;8;7mA\x1b[0mB");
    const Screen& s = t.t.screen();
    const Style& a = s.styles().get(s.row(0).cells[0].styleId());
    EXPECT_TRUE(a.has(Attr::Bold));
    EXPECT_TRUE(a.has(Attr::Italic));
    EXPECT_EQ(a.fg, Color::palette(196));
    EXPECT_EQ(a.bg, Color::rgb(1, 2, 3));
    EXPECT_EQ(a.underline, UnderlineStyle::Curly);
    EXPECT_EQ(a.underlineColor, Color::rgb(9, 8, 7));
    EXPECT_EQ(s.row(0).cells[1].styleId(), StyleTable::DefaultId);
}

TEST(Terminal, StylesAreReleased)
{
    Term t(4, 2, 0);
    for (int i = 0; i < 50; ++i) {
        t.feed("\x1b[38;5;" + std::to_string(i) + "mX");
    }
    t.feed("\x1b[0m\x1b[2J");
    // Only the default style and nothing else stays live once all cells are erased.
    EXPECT_EQ(t.t.screen().styles().liveCount(), 1u);
}

TEST(Terminal, WideCharacters)
{
    Term t(5, 2);
    t.feed("a\xE4\xB8\xAD" "b");
    const Row& r = t.t.screen().row(0);
    EXPECT_EQ(r.cells[1].width(), 2);
    EXPECT_TRUE(r.cells[2].isTail());
    EXPECT_EQ(t.t.screen().cursor.x, 4);
    // overwrite the tail: the head goes too
    t.feed("\x1b[1;3HZ");
    EXPECT_EQ(rowText(t.t, 0), "a Zb");
}

TEST(Terminal, WideCharWrapsAtLastColumn)
{
    Term t(4, 2);
    t.feed("abc\xE4\xB8\xAD");
    EXPECT_TRUE(t.t.screen().row(0).cells[3].isSpacerHead());
    EXPECT_EQ(rowText(t.t, 1), "\xE4\xB8\xAD");
}

TEST(Terminal, GraphemeClusters)
{
    Term t(10, 2);
    t.feed("e\xCC\x81x"); // e + combining acute, x
    EXPECT_EQ(rowText(t.t, 0), "e\xCC\x81x");
    EXPECT_EQ(t.t.screen().cursor.x, 2);
}

TEST(Terminal, Vs16WidensEmoji)
{
    Term t(10, 2);
    t.feed("\xE2\x9D\xA4\xEF\xB8\x8F" "a"); // heart + VS16
    EXPECT_EQ(t.t.screen().row(0).cells[0].width(), 2);
    EXPECT_EQ(t.t.screen().cursor.x, 3);
}

TEST(Terminal, ZwjFamilyIsOneWideCell)
{
    Term t(10, 2);
    t.feed("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7" "a");
    EXPECT_EQ(t.t.screen().cursor.x, 3);
    EXPECT_EQ(t.t.screen().row(0).cells[2].codepoint(), U'a');
}

TEST(Terminal, AltScreen)
{
    Term t(5, 2);
    t.feed("main\x1b[?1049h");
    EXPECT_EQ(screenText(t.t), "");
    // 1049 keeps the cursor where it was, like xterm.
    EXPECT_EQ(t.t.screen().cursor.x, 4);
    t.feed("\x1b[Halt");
    EXPECT_EQ(rowText(t.t, 0), "alt");
    t.feed("\x1b[?1049l");
    EXPECT_EQ(rowText(t.t, 0), "main");
    EXPECT_EQ(t.t.screen().cursor.x, 4);
}

TEST(Terminal, DecSpecialGraphics)
{
    Term t(5, 1);
    t.feed("\x1b(0lqk\x1b(Bq");
    EXPECT_EQ(rowText(t.t, 0), "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90q");
}

TEST(Terminal, Reports)
{
    Term t(10, 5);
    t.feed("\x1b[3;4H\x1b[6n\x1b[c\x1b[>c\x1b[?2026$p\x1b[18t");
    EXPECT_EQ(t.host.written, "\x1b[3;4R\x1b[?65;6;22;28;52c\x1b[>1;10;0c\x1b[?2026;2$y\x1b[8;5;10t");
}

TEST(Terminal, OscTitleAndColors)
{
    Term t(10, 2);
    t.feed("\x1b]2;hello\x07\x1b]11;?\x07\x1b]10;#ff8000\x1b\\\x1b]10;?\x1b\\");
    EXPECT_EQ(t.host.title, "hello");
    EXPECT_EQ(t.host.written, "\x1b]11;rgb:1c1c/1c1c/1c1c\x07\x1b]10;rgb:ffff/8080/0000\x1b\\");
}

TEST(Terminal, RepeatLastCharacter)
{
    Term t(10, 1);
    t.feed("x\x1b[3b");
    EXPECT_EQ(rowText(t.t, 0), "xxxx");
}

TEST(Terminal, TabStops)
{
    Term t(20, 1);
    t.feed("a\tb\x1b[3g\x1b[1;5H\x1bH\r\tc");
    EXPECT_EQ(rowText(t.t, 0), "a   c   b");
}

TEST(Terminal, OriginModeAndMargins)
{
    Term t(10, 5);
    t.feed("\x1b[2;4r\x1b[?6h\x1b[1;1HX\x1b[6n");
    EXPECT_EQ(rowText(t.t, 1), "X");
    EXPECT_EQ(t.host.written, "\x1b[1;2R");
}

TEST(Terminal, LeftRightMargins)
{
    Term t(6, 2);
    t.feed("abcdef\x1b[?69h\x1b[2;4s\x1b[1;2H\x1b[@");
    EXPECT_EQ(rowText(t.t, 0), "a bcef");
}

TEST(Terminal, KittyKeyboardStack)
{
    Term t(10, 2);
    t.feed("\x1b[?u");
    t.feed("\x1b[>1u\x1b[?u");
    t.feed("\x1b[>11u\x1b[?u");
    t.feed("\x1b[=4;2u\x1b[?u");
    t.feed("\x1b[<u\x1b[?u");
    EXPECT_EQ(t.host.written, "\x1b[?0u\x1b[?1u\x1b[?11u\x1b[?15u\x1b[?1u");
    // each screen has its own stack
    t.host.written.clear();
    t.feed("\x1b[?1049h\x1b[?u\x1b[>8u\x1b[?1049l\x1b[?u");
    EXPECT_EQ(t.host.written, "\x1b[?0u\x1b[?1u");
    // plain CSI u is still restore-cursor
    t.feed("\x1b[1;5H\x1b[s\x1b[2;1H\x1b[u");
    EXPECT_EQ(t.t.screen().cursor.x, 4);
}

TEST(Terminal, SetOptionsAppliesLive)
{
    Term t(10, 2);
    t.feed("\x1b]11;#000000\x07\x1b[5 q");
    TerminalOptions o;
    o.defaultBackground = Rgb{1, 2, 3};
    o.cursorShape = CursorShape::Bar;
    o.cursorBlink = false;
    o.maxScrollback = 1;
    t.t.setOptions(o);
    EXPECT_EQ(t.t.defaultBackground(), (Rgb{1, 2, 3}));
    EXPECT_EQ(t.t.cursorShape(), CursorShape::Bar);
    t.feed("\x1b[2 q\x1b[0 q"); // DECSCUSR 0 returns to the configured cursor
    EXPECT_EQ(t.t.cursorShape(), CursorShape::Bar);
    EXPECT_FALSE(t.t.modes().cursorBlink);
    t.feed("a\r\nb\r\nc\r\nd");
    EXPECT_EQ(t.t.screen().historySize(), 1u);
}

TEST(Terminal, Osc8Hyperlinks)
{
    Term t(20, 2);
    t.feed("\x1b]8;id=a;https://example.com\x1b\\link\x1b[1mbold\x1b[0m\x1b]8;;\x1b\\ plain");
    const Screen& s = t.t.screen();
    const auto linkAt = [&](int x) { return s.styles().get(s.row(0).cells[std::size_t(x)].styleId()).link; };
    ASSERT_NE(linkAt(0), 0u);
    EXPECT_EQ(s.styles().linkUri(linkAt(0)), "https://example.com");
    EXPECT_EQ(linkAt(5), linkAt(0)); // SGR changes keep the link
    EXPECT_EQ(linkAt(9), 0u);
    EXPECT_EQ(linkAt(10), 0u);
    // Overwriting the linked cells frees the link.
    t.feed("\x1b[2J");
    EXPECT_EQ(s.styles().liveLinks(), 0u);
}

TEST(Terminal, Xtgettcap)
{
    Term t(10, 2);
    // "TN" = 544E, "Tc" = 5463, "zz" = 7A7A
    t.feed("\x1bP+q544E;5463;7A7A\x1b\\");
    EXPECT_EQ(t.host.written, "\x1bP1+r544E=6166746572676C6F77\x1b\\\x1bP1+r5463\x1b\\\x1bP0+r7A7A\x1b\\");
}

TEST(Terminal, Decrqss)
{
    Term t(10, 4);
    t.feed("\x1b[1;4:3;31;48;2;1;2;3m\x1bP$qm\x1b\\");
    EXPECT_EQ(t.host.written, "\x1bP1$r0;1;4:3;31;48:2::1:2:3m\x1b\\");
    t.host.written.clear();
    t.feed("\x1b[2;3r\x1bP$qr\x1b\\\x1b[6 q\x1bP$q q\x1b\\\x1bP$qx\x1b\\");
    EXPECT_EQ(t.host.written, "\x1bP1$r2;3r\x1b\\\x1bP1$r6 q\x1b\\\x1bP0$r\x1b\\");
}

TEST(Terminal, NotificationsAndPointer)
{
    Term t(10, 2);
    t.feed("\x1b]9;build done\x07");
    EXPECT_EQ(t.host.notified, "|build done");
    t.feed("\x1b]9;4;1;50\x07"); // progress, not a message
    EXPECT_EQ(t.host.notified, "|build done");
    t.feed("\x1b]777;notify;Tests;all passed\x1b\\");
    EXPECT_EQ(t.host.notified, "Tests|all passed");
    t.feed("\x1b]22;pointer\x1b\\");
    EXPECT_EQ(t.host.pointer, "pointer");
}
