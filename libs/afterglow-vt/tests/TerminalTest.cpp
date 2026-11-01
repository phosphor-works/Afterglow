// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

#include <AfterglowVt/afterglowvt_version.h>

#include <gtest/gtest.h>

#include <clocale>
#include <string>
#include <vector>

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
    ASSERT_GT(t.t.screen().styles().liveCount(), 1u); // the cells hold styles now
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

TEST(Terminal, WideOverwriteClearsFormerTailGrapheme)
{
    Term t(10, 2);
    t.feed("aX\xcc\x81\r界\x1b[1;2Hb\xcc\x80");
    const auto& screen = t.t.screen();
    EXPECT_EQ(screen.graphemeExtra(screen.row(0), 1), U"\u0300");
    EXPECT_EQ(rowText(t.t, 0), " b\xcc\x80");
}

TEST(Terminal, AbsoluteAxisMovementRespectsMarginsAndLineWidth)
{
    Term t(10, 5);
    t.feed("\x1b[2;4r\x1b[?69h\x1b[3;7s\x1b[?6h\x1b[99G\x1b[99d");
    EXPECT_EQ(t.t.screen().cursor.x, 6);
    EXPECT_EQ(t.t.screen().cursor.y, 3);
    t.feed("\x1b[1`\x1b[1d");
    EXPECT_EQ(t.t.screen().cursor.x, 2);
    EXPECT_EQ(t.t.screen().cursor.y, 1);
    t.feed("\x1b[?6l\x1b[?69l\x1b[r\x1b[H\x1b#6\x1b[99G");
    EXPECT_EQ(t.t.screen().cursor.x, 4);
    t.feed("\x1b[99`");
    EXPECT_EQ(t.t.screen().cursor.x, 4);
    t.feed("\x1b[2;10H\x1b[1d");
    EXPECT_EQ(t.t.screen().cursor.x, 4);
}

TEST(Terminal, CursorColorQueryMatchesConfiguredOverrideAndReset)
{
    Term t(10, 2);
    TerminalOptions options;
    options.cursorColor = Rgb{1, 2, 3};
    t.t.setOptions(options);
    t.feed("\x1b]12;?\a\x1b]12;#102030\a\x1b]12;?\a\x1b]112\a\x1b]12;?\a");
    EXPECT_EQ(t.host.written, "\x1b]12;rgb:0101/0202/0303\a\x1b]12;rgb:1010/2020/3030\a\x1b]12;rgb:0101/0202/0303\a");
    t.host.written.clear();
    t.feed("\x1b]21;cursor=\a\x1b]10;#112233\a\x1b]12;?\a");
    EXPECT_EQ(t.host.written, "\x1b]12;rgb:1111/2222/3333\a");
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
    EXPECT_EQ(t.host.written, "\x1b[3;4R\x1b[?65;4;6;22;28;52c\x1b[>1;10;0c\x1b[?2026;2$y\x1b[8;5;10t");
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
    t.feed("\x1b]11;#000000\x07\x1b[1 q"); // a blinking block
    TerminalOptions o;
    o.defaultBackground = Rgb{1, 2, 3};
    o.cursorShape = CursorShape::Bar;
    o.cursorBlink = false;
    o.maxScrollback = 1;
    t.t.setOptions(o);
    EXPECT_EQ(t.t.defaultBackground(), (Rgb{1, 2, 3}));
    EXPECT_EQ(t.t.cursorShape(), CursorShape::Bar); // the configured cursor changed
    EXPECT_FALSE(t.t.modes().cursorBlink);
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

TEST(Terminal, ShellIntegrationCommands)
{
    RecordingHost host;
    Terminal t(20, 6, host);
    t.feed("\x1b]133;A\x07$ \x1b]133;B\x07make test\r\n\x1b]133;C\x07");
    t.feed("ok 1\r\nok 2\r\n\x1b]133;D;2\x07");
    ASSERT_EQ(host.commandsStarted.size(), 1u);
    EXPECT_EQ(host.commandsStarted[0], "make test");
    ASSERT_EQ(host.commandsFinished.size(), 1u);
    EXPECT_EQ(host.commandsFinished[0], 2);
    EXPECT_EQ(t.text(Terminal::TextScope::LastOutput), "ok 1\nok 2\n");
    // A new prompt without D finishes the command with an unknown code.
    t.feed("\x1b]133;A\x07$ \x1b]133;B\x07sleep 9\r\n\x1b]133;C\x07\x1b]133;A\x07");
    ASSERT_EQ(host.commandsFinished.size(), 2u);
    EXPECT_EQ(host.commandsFinished[1], -1);
    EXPECT_EQ(t.text(Terminal::TextScope::Screen), "$ make test\nok 1\nok 2\n$ sleep 9");
}

TEST(Terminal, ClearScreenAndHistory)
{
    RecordingHost host;
    Terminal t(10, 3, host);
    t.feed("one\r\ntwo\r\nthree\r\nfour\r\n$ ");
    ASSERT_GT(t.screen().historySize(), 0u);
    t.clearScreenAndHistory();
    EXPECT_EQ(t.screen().historySize(), 0u);
    EXPECT_EQ(t.text(Terminal::TextScope::All), "");
    // A full-screen program keeps its screen; only the main history goes.
    t.feed("a\r\nb\r\nc\r\nd\r\n\x1b[?1049hTUI");
    t.clearScreenAndHistory();
    EXPECT_EQ(t.text(Terminal::TextScope::Screen), "\n\nTUI"); // untouched
    t.feed("\x1b[?1049l");
    EXPECT_EQ(t.screen().historySize(), 0u);
}

TEST(Terminal, RepeatLastCharacterRuns)
{
    RecordingHost host;
    Terminal t(6, 3, host);
    t.feed("ab\x1b[3b");
    EXPECT_EQ(t.text(Terminal::TextScope::Screen), "abbbb");
    // Across the end of the line, wrapping as printing would.
    t.feed("\x1b[2J\x1b[Hxy\x1b[6b");
    EXPECT_EQ(t.text(Terminal::TextScope::Screen), "xyyyyyyy");
    // A DEC line-drawing character repeats as drawn.
    t.feed("\x1b[2J\x1b[H\x1b(0q\x1b[2b\x1b(B");
    EXPECT_EQ(t.text(Terminal::TextScope::Screen), "───");
    // Insert mode pushes the rest of the line along.
    t.feed("\x1b[2J\x1b[Hzz\x1b[H\x1b[4hk\x1b[2b\x1b[4l");
    EXPECT_EQ(t.text(Terminal::TextScope::Screen), "kkkzz");
}

TEST(Terminal, BatchedPrintingMatchesBytewise)
{
    // Fed whole, text takes the batched fast paths (ASCII runs, decoded
    // UTF-8 runs, simple characters). Fed a byte at a time, multi-byte UTF-8
    // and anything after a broken sequence take the state machine and
    // print(); ASCII still takes the run path, one byte long. Strings (OSC,
    // DCS, APC) take the payload fast path whole and the state machine byte
    // by byte. The screens must come out the same.
    const std::string inputs[] = {
        "plain ascii that wraps past the edge of this narrow terminal\r\n",
        "Ελληνικά кириллица 日本語の文字 한국어 \xed\x95\x9c\xea\xb8\x80 wide at the edge: 中中中中中中中中中中中\r\n",
        "e\xcc\x81 n\xcc\x83 a\xcc\x8a\xcc\x8a combining after simple letters\r\n",
        "flags \xf0\x9f\x87\xaf\xf0\x9f\x87\xb5\xf0\x9f\x87\xba\xf0\x9f\x87\xb8 zwj \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9 vs16 \xe2\x9d\xa4\xef\xb8\x8f\r\n",
        "prepend \xd8\x80" "a then devanagari \xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7 hangul \xe1\x84\x80\xe1\x85\xa1\xe1\x86\xa8\r\n",
        "\x1b[31mred \xc3\xa9t\xc3\xa9\x1b[0m \x1b[1mbold \xce\xb1\xce\xb2\x1b[m\r\n",
        "bad \xc3 \xe2\x82 \xed\xa0\x80 \xc0\xaf \xf4\x90\x80\x80 \xff end\r\n",
        "\x1b[4h insert \xc3\xa9\xc3\xa9 \x1b[4l \x1b(0lqqk\x1b(B mixed\r\n",
        "\x1b]2;title\x1b\\\x1bP$qm\x1b\\\x1b_Ga=q,i=1;AAAA\x1b\\\x1b]8;;https://x.example\x07link\x1b]8;;\x07\r\n",
    };
    for (const std::string& input : inputs) {
        RecordingHost hostA;
        RecordingHost hostB;
        Terminal whole(17, 6, hostA);
        Terminal bytewise(17, 6, hostB);
        whole.feed(input);
        for (char c : input) {
            bytewise.feed(std::string_view(&c, 1));
        }
        ASSERT_EQ(whole.text(Terminal::TextScope::All), bytewise.text(Terminal::TextScope::All)) << input;
        for (int y = 0; y < whole.rows(); ++y) {
            for (int x = 0; x < whole.cols(); ++x) {
                const Cell& a = whole.screen().row(y).cells[std::size_t(x)];
                const Cell& b = bytewise.screen().row(y).cells[std::size_t(x)];
                ASSERT_EQ(a.codepoint(), b.codepoint()) << input << " at " << x << "," << y;
                ASSERT_EQ(a.width(), b.width()) << input << " at " << x << "," << y;
                ASSERT_EQ(a.hasGrapheme(), b.hasGrapheme()) << input << " at " << x << "," << y;
                ASSERT_EQ(a.isSpacerHead(), b.isSpacerHead()) << input << " at " << x << "," << y;
                ASSERT_EQ(whole.screen().styles().get(a.styleId()), bytewise.screen().styles().get(b.styleId()))
                    << input << " at " << x << "," << y;
                ASSERT_EQ(whole.screen().graphemeExtra(whole.screen().row(y), x),
                          bytewise.screen().graphemeExtra(bytewise.screen().row(y), x))
                    << input << " at " << x << "," << y;
            }
            ASSERT_EQ(whole.screen().row(y).wrapped, bytewise.screen().row(y).wrapped) << input << " row " << y;
        }
        EXPECT_EQ(hostA.written, hostB.written) << input;
        EXPECT_EQ(hostA.title, hostB.title) << input;
        EXPECT_EQ(whole.screen().cursor.x, bytewise.screen().cursor.x) << input;
        EXPECT_EQ(whole.screen().cursor.y, bytewise.screen().cursor.y) << input;
    }
}

TEST(Terminal, AmbiguousWidthFollowsTheOption)
{
    Term narrow(10, 2);
    narrow.t.feed("\xce\xb1\xce\xb2x"); // αβx
    EXPECT_EQ(narrow.t.screen().cursor.x, 3);

    Term wide(10, 2);
    TerminalOptions o;
    o.ambiguousWide = true;
    wide.t.setOptions(o);
    wide.t.feed("\xce\xb1\xce\xb2x");
    EXPECT_EQ(wide.t.screen().cursor.x, 5);
    EXPECT_EQ(wide.t.screen().row(0).at(0).width(), 2);
    EXPECT_EQ(wide.t.screen().row(0).at(2).codepoint(), U'\u03b2');
}

TEST(Terminal, BidiControlsAreRetainedAndCopiedOnlyForBidiShellContent)
{
    for (BidiMode mode : {BidiMode::Auto, BidiMode::LeftToRight, BidiMode::RightToLeft}) {
        RecordingHost host;
        TerminalOptions options;
        options.bidi = mode;
        Terminal terminal(20, 3, host, options);
        const std::string control = "\xe2\x80\xae";
        terminal.feed(control + "abc");
        EXPECT_EQ(terminal.screen().row(0).at(0).codepoint(), U'\u202e');
        EXPECT_EQ(terminal.screen().cursor.x, 4);
        EXPECT_EQ(terminal.text(Terminal::TextScope::All), control + "abc");
        terminal.feed("\x1b[?1049h\x1b[H" + control + "x");
        EXPECT_EQ(terminal.screen().row(0).at(0).codepoint(), U'x');
        terminal.feed("\x1b[?1049l\x1b[2J\x1b[H\x1b[?1000h" + control + "y");
        EXPECT_EQ(terminal.screen().row(0).at(0).codepoint(), U'y');
    }
    // Bidi off: the control takes no cell, as everywhere else.
    RecordingHost host;
    Terminal off(20, 3, host);
    off.feed("\xe2\x80\xae" "abc");
    EXPECT_EQ(off.screen().row(0).at(0).codepoint(), U'a');
    EXPECT_EQ(off.text(Terminal::TextScope::All), "abc");
}

TEST(Terminal, SemanticRegionsRetainTextThroughReflow)
{
    Term t(10, 5);
    t.feed("\x1b]133;A\a$ \x1b]133;B\aprint abc\x1b]133;C\a\r\nabcdefghijklmno\x1b]133;D;0\a");
    auto regions = t.t.semanticRegions();
    ASSERT_EQ(regions.size(), 3u);
    EXPECT_EQ(t.t.semanticRegionText(regions[0].id), "$" );
    EXPECT_EQ(t.t.semanticRegionText(regions[1].id), "print abc");
    EXPECT_EQ(t.t.semanticRegionText(regions[2].id), "\nabcdefghijklmno");
    t.t.resize(6, 5, 10, 20);
    EXPECT_EQ(t.t.semanticRegionText(regions[1].id), "print abc");
    EXPECT_EQ(t.t.semanticRegionText(regions[2].id), "\nabcdefghijklmno");
    EXPECT_EQ(t.t.semanticRegions().back().exitCode, 0);
    bool truncated = false;
    EXPECT_EQ(t.t.semanticRegionText(regions[2].id, 4, &truncated), "\nabc");
    EXPECT_TRUE(truncated);
    t.t.reset();
    t.feed("\x1b]133;A\a");
    EXPECT_GT(t.t.semanticRegions().front().id, regions.back().id);
}

TEST(Terminal, SemanticRegionsWhoseRowsAResizeDropsAreGone)
{
    Term t(10, 3, 1);
    t.feed("\x1b]133;A\a$ \x1b]133;B\aecho\x1b]133;C\a\r\n0123456789\x1b]133;D;0\a\r\nabcdefghij\r\nklmnopqrst");
    ASSERT_EQ(t.t.text(Terminal::TextScope::LastOutput), "\n0123456789");
    // Narrower, every line takes two rows: the command's rows leave the buffer.
    t.t.resize(5, 3, 10, 20);
    const auto regions = t.t.semanticRegions();
    ASSERT_EQ(regions.size(), 3u);
    for (const auto& region : regions) {
        EXPECT_FALSE(region.available);
        EXPECT_TRUE(region.startTrimmed);
        EXPECT_EQ(t.t.semanticRegionText(region.id), std::nullopt);
    }
    EXPECT_EQ(t.t.text(Terminal::TextScope::LastOutput), "");
}

TEST(Terminal, SemanticRegionsKeepTheNewest1024)
{
    Term t(20, 5);
    t.feed("\x1b]133;A\a");
    const std::uint64_t first = t.t.semanticRegions().front().id;
    for (int i = 0; i < 1099; ++i) {
        t.feed("\x1b]133;A\a");
    }
    const auto regions = t.t.semanticRegions();
    ASSERT_EQ(regions.size(), 1024u);
    EXPECT_EQ(regions.front().id, first + 76);
    EXPECT_EQ(regions.back().id, first + 1099);
}

TEST(Terminal, ShellMarksMoveWithInsertedAndDeletedLines)
{
    Term t(20, 6);
    t.feed("\x1b[3;1H\x1b]133;A\a$ \x1b]133;B\aecho hi\x1b]133;C\a\r\nhi\x1b]133;D;0\a");
    const auto regions = t.t.semanticRegions();
    ASSERT_EQ(regions.size(), 3u);
    ASSERT_EQ(t.t.text(Terminal::TextScope::LastOutput), "\nhi");
    t.feed("\x1b[1;1H\x1b[2L"); // IL 2 at the top: the command moves to rows 4 and 5
    EXPECT_EQ(t.t.text(Terminal::TextScope::LastOutput), "\nhi");
    EXPECT_EQ(t.t.semanticRegionText(regions[1].id), "echo hi");
    EXPECT_EQ(t.t.semanticRegionText(regions[2].id), "\nhi");
    // DL 4 from row 1: the prompt row leaves the region, its output's last row
    // comes up to row 1. The regions and marks that start on the prompt row go.
    t.feed("\x1b[2;1H\x1b[4M");
    EXPECT_EQ(rowText(t.t, 1), "hi");
    EXPECT_TRUE(t.t.semanticRegions().empty());
    EXPECT_EQ(t.t.text(Terminal::TextScope::LastOutput), "");
}

TEST(Terminal, SavedCursorRetainsHyperlinkAndAltScreenReinternsIt)
{
    Term t(20, 4);
    t.feed("\x1b]8;id=one;https://one.example\a\x1b" "7\x1b]8;;\a");
    t.feed("\x1b]8;id=two;https://two.example\aX\x1b]8;;\a\x1b" "8A");
    const auto& screen = t.t.screen();
    EXPECT_EQ(screen.styles().linkUri(screen.styles().get(screen.row(0).at(0).styleId()).link), "https://one.example");
    t.feed("\x1b[?1049hB");
    const auto& alt = t.t.screen();
    EXPECT_EQ(alt.styles().linkUri(alt.styles().get(alt.row(0).at(1).styleId()).link), "https://one.example");
}

TEST(Terminal, ScreenTextJoinsSoftWrapsAndKeepsWrappedSpaces)
{
    Term t(5, 3);
    t.feed("abc  def");
    EXPECT_EQ(t.t.text(Terminal::TextScope::Screen), "abc  def");
}

TEST(Terminal, KeycapWidensAndSemanticEndIncludesRightmostCell)
{
    Term t(5, 3);
    t.feed("1\xef\xb8\x8f\xe2\x83\xa3");
    EXPECT_EQ(t.t.screen().row(0).at(0).width(), 2);
    t.t.reset();
    t.feed("\x1b]133;C\a12345\x1b]133;D;0\a");
    EXPECT_EQ(t.t.text(Terminal::TextScope::LastOutput), "12345");
    t.t.resize(3, 3, 10, 20);
    EXPECT_EQ(t.t.semanticRegionText(t.t.semanticRegions().back().id), "12345");
}

TEST(Terminal, ReloadKeepsTheProgramsCursorAndDropsItsColours)
{
    Term t(10, 2);
    t.feed("\x1b[6 q"); // a steady bar, as vim's insert mode asks
    t.feed("\x1b]17;#102030\x07\x1b]4;1;#ff0000\x07\x1b[#P\x1b]4;1;#00ff00\x07");
    TerminalOptions o = t.t.options();
    o.maxScrollback = 5; // unrelated to the cursor and the colours
    t.t.setOptions(o);
    EXPECT_EQ(t.t.cursorShape(), CursorShape::Bar);
    EXPECT_FALSE(t.t.modes().cursorBlink);
    EXPECT_EQ(t.t.selectionBackground(), nullptr);
    const Rgb base = t.t.paletteColor(1);
    t.feed("\x1b[#Q"); // the stack went with the reload: nothing to pop
    EXPECT_EQ(t.t.paletteColor(1), base);
}

TEST(Terminal, MulticellWalkBelowTheScrollRegionEnds)
{
    // The lower half of a full-width row of blocks on the last row, below
    // the scroll region: index() cannot move, so after its one wrap the walk
    // crosses the same row again and erases the block that does not fit.
    for (const bool bytewise : {false, true}) {
        Term t(80, 24);
        const std::string in = "\x1b[23;1H\x1b]66;s=2;" + std::string(40, 'A') + "\x1b\\\x1b[1;10r\x1b[24;1HB";
        if (bytewise) {
            for (char c : in) {
                t.feed(std::string_view(&c, 1));
            }
        } else {
            t.feed(in);
        }
        EXPECT_EQ(rowText(t.t, 23), std::string(78, ' ') + "B");
        EXPECT_FALSE(t.t.screen().row(22).at(78).isMulticell());
        EXPECT_FALSE(t.t.screen().row(23).at(78).isMulticell());
        EXPECT_TRUE(t.t.screen().row(23).at(0).isMulticell()); // the blocks it walked past stay
    }
}

namespace {
/// A 1x1 kitty image placed at the cursor (RGB, so no decoder is needed).
constexpr std::string_view KittyPixel = "\x1b_Ga=T,f=24,s=1,v=1;AAAA\x1b\\";
} // namespace

TEST(Terminal, ResetDuringACommandKeepsItsFinish)
{
    // RIS (as `reset` sends it) in the middle of a command: the shell's D
    // still finishes the command.
    Term t(20, 4);
    t.feed("\x1b]133;A\x07\x1b]133;B\x07\x1b]133;C\x07\x1b" "c\x1b]133;D;0\x07");
    ASSERT_EQ(t.host.commandsStarted.size(), 1u);
    EXPECT_EQ(t.host.commandsFinished, std::vector<int>{0});
}

TEST(Terminal, CommandFinishesWhileTheAlternateScreenIsActive)
{
    // A full-screen program killed without leaving 1049 keeps its exit code.
    Term t(20, 4);
    t.feed("\x1b]133;C\x07\x1b[?1049h\x1b]133;D;137\x07\x1b]133;A\x07");
    EXPECT_EQ(t.host.commandsFinished, std::vector<int>{137});
}

TEST(Terminal, CommandWithoutInputMarkIsEmpty)
{
    Term t(20, 6);
    t.feed("\x1b]133;A\x07$ \x1b]133;B\x07ls\r\n\x1b]133;C\x07out\r\n\x1b]133;D;0\x07");
    t.feed("\x1b]133;A\x07$  \x1b]133;C\x07"); // no B in this cycle
    ASSERT_EQ(t.host.commandsStarted.size(), 2u);
    EXPECT_EQ(t.host.commandsStarted[0], "ls");
    EXPECT_EQ(t.host.commandsStarted[1], "");
}

TEST(Terminal, EncodedCommandLinesLoseControlBytes)
{
    Term t(20, 4);
    t.feed("\x1b]133;C;cmdline_url=%1b%5d2%3bx%07%0aecho%-1\x07");
    ASSERT_EQ(t.host.commandsStarted.size(), 1u);
    EXPECT_EQ(t.host.commandsStarted[0], "]2;x\necho%-1"); // ESC and BEL gone; "%-1" is no escape
    t.feed("\x1b]633;E;a\\x1bb\\x-1\x07\x1b]133;D\x07\x1b]633;C\x07");
    ASSERT_EQ(t.host.commandsStarted.size(), 2u);
    EXPECT_EQ(t.host.commandsStarted[1], "ab\\x-1");
}

TEST(Terminal, WorkingDirectoryReportsOnlyChanges)
{
    Term t(20, 4);
    t.feed("\x1b]7;file://host/tmp\x07\x1b]7;file://host/tmp\x07");
    EXPECT_EQ(t.host.cwd, "file://host/tmp"); // OSC 7 passes its URI on as sent
    EXPECT_EQ(t.host.cwdChanges, 1);
    t.feed("\x1b]633;P;Cwd=/a\\x0ab\x07"); // a path with a control byte is refused whole
    EXPECT_EQ(t.host.cwd, "file://host/tmp");
    t.feed("\x1b]1337;CurrentDir=/srv\x07");
    EXPECT_EQ(t.host.cwd, "/srv");
    EXPECT_EQ(t.host.cwdChanges, 2);
}

TEST(Terminal, SavedCursorFollowsReflow)
{
    // vim on the alternate screen while the window widens: leaving it, the
    // cursor goes back to the prompt it left, wherever reflow put it.
    Term t(10, 5);
    t.feed("abcdefghijKLMNO\r\n$ ");
    t.feed("\x1b[?1049h");
    t.t.resize(20, 5, 8, 16);
    t.feed("\x1b[?1049l");
    const Cursor& cursor = t.t.screen().cursor;
    EXPECT_EQ(rowText(t.t, cursor.y), "$");
    EXPECT_EQ(cursor.x, 2);
}

TEST(Terminal, SavedCursorKeepsItsPendingWrapThroughReflow)
{
    // The cursor sat after the last column when it was saved: after a wider
    // reflow the next character goes after the line, not over its end.
    Term alt(10, 5);
    alt.feed("abcdefghij\x1b[?1049h");
    alt.t.resize(20, 5, 8, 16);
    alt.feed("\x1b[?1049lk");
    EXPECT_EQ(rowText(alt.t, 0), "abcdefghijk");
    Term saved(80, 5);
    saved.feed(std::string(80, 'a') + "\x1b" "7");
    saved.t.resize(100, 5, 8, 16);
    saved.feed("\x1b" "8b");
    EXPECT_EQ(rowText(saved.t, 0), std::string(80, 'a') + "b");
}

TEST(Terminal, ScreenSwitchCarriesThePendingWrap)
{
    Term t(80, 24);
    t.feed("\x1b[?1049h\x1b[1;80HZ\x1b[?1049l\x1b[5;5H\x1b[?1049hW");
    EXPECT_EQ(t.t.screen().row(4).at(4).codepoint(), U'W');
    Term primary(80, 24);
    primary.feed("\x1b[1;80HX\x1b[?47h\x1b[5;5H\x1b[1m\x1b[?47lY");
    EXPECT_EQ(primary.t.screen().row(4).at(4).codepoint(), U'Y');
    // The rendition is the terminal's, not a screen's.
    const auto& styles = primary.t.screen().styles();
    EXPECT_TRUE(styles.get(primary.t.screen().row(4).at(4).styleId()).has(Attr::Bold));
}

TEST(Terminal, ClearForgetsTheLastPrintedCell)
{
    Term t(10, 3);
    t.feed("e");
    t.t.clearScreenAndHistory();
    t.feed("\xcc\x81"); // a combining mark with nothing before the cursor
    EXPECT_FALSE(t.t.screen().row(0).at(0).hasGrapheme());
    EXPECT_EQ(t.t.text(Terminal::TextScope::All), "");
}

TEST(Terminal, XtversionReportsTheProjectVersion)
{
    Term t(10, 3);
    t.feed("\x1b[>q");
    EXPECT_EQ(t.host.written, std::string("\x1bP>|afterglow ") + AFTERGLOW_VT_VERSION + "\x1b\\");
}

TEST(Terminal, PromptRedrawWithoutScrollback)
{
    // A width change reflows with or without history, so the shell's
    // repaint of its prompt is awaited in both.
    Term t(10, 3, 0);
    t.feed("\x1b]133;A\x07$ ");
    t.t.resize(12, 3, 8, 16);
    EXPECT_TRUE(t.t.promptRedrawPending());
    Term kept(10, 3);
    kept.feed("\x1b]133;A\x07$ ");
    kept.t.resize(12, 3, 8, 16);
    EXPECT_TRUE(kept.t.promptRedrawPending());
}

TEST(Terminal, ResizeWithoutScrollbackKeepsImagesAndMarks)
{
    Term t(10, 3, 0);
    t.t.resize(10, 3, 8, 16);
    t.feed("0123456789AB\x1b]133;A\x07");
    t.feed(KittyPixel);
    t.t.resize(5, 3, 8, 16);
    EXPECT_EQ(rowText(t.t, 0), "01234");
    EXPECT_EQ(t.t.graphics().placements().size(), 1u);
    EXPECT_EQ(t.t.semanticRegions().size(), 1u);
}

TEST(Terminal, MouseFormatResetsOnlyItsOwn)
{
    Term t(10, 2);
    t.feed("\x1b[?1000h\x1b[?1006h\x1b[?1015l");
    EXPECT_EQ(t.t.modes().mouseFormat, MouseFormat::Sgr);
    t.feed("\x1b[?1005l\x1b[?1016l");
    EXPECT_EQ(t.t.modes().mouseFormat, MouseFormat::Sgr);
    t.feed("\x1b[?1006l");
    EXPECT_EQ(t.t.modes().mouseFormat, MouseFormat::X10);
    t.feed("\x1b[?1015h\x1b[?1015l");
    EXPECT_EQ(t.t.modes().mouseFormat, MouseFormat::X10);
}

TEST(Terminal, DoubleWidthLinesKeepTheCursorVisible)
{
    // Moves that land in the hidden right half come back into the visible one.
    Term t(80, 4);
    t.feed("\x1b#6\x1b[10I"); // CHT
    EXPECT_LE(t.t.screen().cursor.x, 39);
    t.feed("\xe3\x81\x82"); // a wide character at the edge of the visible half
    EXPECT_NE(t.t.screen().row(0).at(79).width(), 2);
    Term wrap(80, 4);
    wrap.feed("\x1b#6\x1b[2;1H\x1b[?1045h\bA"); // reverse wrap into the line above
    EXPECT_EQ(wrap.t.screen().row(0).at(39).codepoint(), U'A');
    EXPECT_TRUE(wrap.t.screen().row(0).at(79).isEmpty());
    Term restore(80, 4);
    restore.feed("\x1b[1;70H\x1b" "7\x1b#6\x1b" "8"); // DECRC onto a line made double width since
    EXPECT_LE(restore.t.screen().cursor.x, 39);
    Term vt52(80, 4);
    vt52.feed("\x1b[2;1H\x1b#6\x1b[1;70H\x1b[?2l\x1b" "B"); // VT52 cursor down onto it
    EXPECT_EQ(vt52.t.screen().cursor.y, 1);
    EXPECT_LE(vt52.t.screen().cursor.x, 39);
    vt52.feed("\x1b<\x1b[3;70H\x1b[?2l\x1b" "A"); // and up onto it
    EXPECT_EQ(vt52.t.screen().cursor.y, 1);
    EXPECT_LE(vt52.t.screen().cursor.x, 39);
}

TEST(Terminal, Vs16WideningRespectsInsertMarginsAndDoubleWidth)
{
    const std::string heart = "\xe2\x9d\xa4\xef\xb8\x8f";
    Term insert(10, 2);
    insert.feed("abc\x1b[1G\x1b[4h" + heart);
    EXPECT_EQ(insert.t.screen().row(0).at(0).width(), 2);
    EXPECT_EQ(insert.t.screen().row(0).at(2).codepoint(), U'a'); // shifted, not overwritten
    EXPECT_EQ(insert.t.screen().row(0).at(4).codepoint(), U'c');
    Term margins(80, 2);
    margins.feed("\x1b[?69h\x1b[1;10s\x1b[1;21H" + heart + "A"); // right of the margin
    EXPECT_EQ(margins.t.screen().row(0).at(22).codepoint(), U'A');
    EXPECT_FALSE(margins.t.screen().row(0).wrapped);
    Term doubled(80, 2);
    doubled.feed("\x1b#6\x1b[1;40H" + heart); // the visible half's last cell: no room
    EXPECT_EQ(doubled.t.screen().row(0).at(39).width(), 1);
}

TEST(Terminal, ColumnModeReportsTheWidthTheHostGave)
{
    Term t(80, 4);
    t.t.resize(80, 4, 10, 20);
    t.feed("\x1b[?40h");
    t.feed(KittyPixel);
    t.feed("\x1b[>2;2:1:1 q\x1b[?3h\x1b[?3$p"); // the host refuses every resize
    EXPECT_EQ(t.t.cols(), 80);
    EXPECT_EQ(t.host.written.substr(t.host.written.rfind("\x1b[?3;")), "\x1b[?3;2$y");
    // The page was cleared: its image and extra cursors went with the text.
    EXPECT_TRUE(t.t.graphics().placements().empty());
    EXPECT_TRUE(t.t.extraCursors().empty());
}

TEST(Terminal, HistoryClearsKeepImagesOnTheScreen)
{
    Term t(10, 3);
    t.t.resize(10, 3, 10, 20);
    t.feed(KittyPixel);
    t.feed("\x1b[3J"); // ED 3: the saved lines only
    EXPECT_EQ(t.t.graphics().placements().size(), 1u);
    t.feed("\r\n\r\n\r\n\r\n\x1b[3J"); // now in history: it goes
    EXPECT_TRUE(t.t.graphics().placements().empty());
    t.feed(KittyPixel);
    t.feed("\r\n\r\n\r\n\r\n\x1b]1337;ClearScrollback\x07");
    EXPECT_TRUE(t.t.graphics().placements().empty());
    t.feed(KittyPixel);
    t.feed("\r\n\r\n\r\n\r\n\x1b[?1049h");
    t.t.clearScreenAndHistory();
    t.feed("\x1b[?1049l");
    EXPECT_TRUE(t.t.graphics().placements().empty());
}

TEST(Terminal, PointerShapeReportsOnlyChanges)
{
    Term t(10, 2);
    t.feed("\x1b]22;=bogus\x07\x1b]22;=bogus\x07");
    EXPECT_EQ(t.host.pointerChanges, 0);
    t.feed("\x1b]22;pointer\x07\x1b]22;=pointer\x07");
    EXPECT_EQ(t.host.pointerChanges, 1);
    EXPECT_EQ(t.host.pointer, "pointer");
}

TEST(Terminal, ExtraCursorsAreBounded)
{
    Term t(100, 50);
    t.feed("\x1b[>1;4 q"); // 5,000 cells
    EXPECT_EQ(t.t.extraCursors().size(), Terminal::MaxExtraCursors);
    std::string repeated = "\x1b[>2";
    for (int i = 0; i < 4000; ++i) {
        repeated += ";4";
    }
    t.feed(repeated + " q"); // thousands of full-screen groups count once
    EXPECT_EQ(t.t.extraCursors().size(), Terminal::MaxExtraCursors);
    EXPECT_EQ(t.t.extraCursors().begin()->second, 2);
    t.feed("\x1b[>0;4:1:1:1:100 q"); // erase the top row
    EXPECT_EQ(t.t.extraCursors().count({0, 0}), 0u);
    t.feed("\x1b[>0;4 q");
    EXPECT_TRUE(t.t.extraCursors().empty());
}

TEST(Terminal, RepeatedExtraCursorGroupsOnASmallScreenCountOnce)
{
    // 1,920 cells: the cursor limit never stops the walk here.
    Term t(80, 24);
    std::string repeated = "\x1b[>1";
    for (int i = 0; i < 400; ++i) { // 3,601 parameters, under the parser's limit
        repeated += ";4;4:1:1:2:2;2:3:3";
    }
    t.feed(repeated + " q");
    EXPECT_EQ(t.t.extraCursors().size(), 80u * 24u);
    EXPECT_EQ(t.t.extraCursors().at({2, 2}), 1);
    t.feed("\x1b[>0;4:1:1:2:2;4:1:1:2:2;2:3:3 q");
    EXPECT_EQ(t.t.extraCursors().size(), 80u * 24u - 5u);
    EXPECT_EQ(t.t.extraCursors().count({1, 1}), 0u);
    EXPECT_EQ(t.t.extraCursors().count({2, 2}), 0u);
    EXPECT_EQ(t.t.extraCursors().count({2, 3}), 1u);
}

TEST(Terminal, ResizeClipsExtraCursorsToTheClampedSize)
{
    Term t(10, 5);
    t.feed("\x1b[>1;2:1:2 q"); // row 1, column 2
    t.t.resize(1, 5, 8, 16); // one column becomes two
    EXPECT_EQ(t.t.extraCursors().size(), 1u);
}

TEST(Terminal, ListRepliesAreBounded)
{
    Term t(10, 2);
    std::string queries = "\x1b]4";
    for (int i = 0; i < 300; ++i) {
        queries += ";" + std::to_string(i % 256) + ";?";
    }
    t.feed(queries + "\x07");
    auto replies = [&](std::string_view marker) {
        std::size_t count = 0;
        for (std::size_t pos = t.host.written.find(marker); pos != std::string::npos;
             pos = t.host.written.find(marker, pos + 1)) {
            ++count;
        }
        return count;
    };
    EXPECT_EQ(replies("\x1b]4;"), 256u);
    t.host.written.clear();
    std::string names = "\x1bP+q";
    for (int i = 0; i < 300; ++i) {
        names += "544E;";
    }
    t.feed(names + "544E\x1b\\");
    EXPECT_EQ(replies("\x1bP1+r"), 256u);
    t.host.written.clear();
    t.feed("\x1b]22;?" + std::string(5000, ',') + "\x07"); // over its limit: no reply at all
    t.feed("\x1b]21" + std::string(70000, ';') + "\x07");
    EXPECT_EQ(t.host.written, "");
}

TEST(Terminal, CapabilityRepliesNeverEchoControlBytes)
{
    Term t(10, 2);
    t.feed("\x1bP+q4141\r touch /tmp/pwned\r\x1b\\\x1bP+q414\x1b\\");
    EXPECT_EQ(t.host.written, "\x1bP0+r\x1b\\\x1bP0+r\x1b\\");
    t.host.written.clear();
    t.feed("\x1bP+q5365\x1b\\"); // Se: back to the configured cursor
    EXPECT_EQ(t.host.written, "\x1bP1+r5365=1B5B302071\x1b\\");
}

TEST(Terminal, ClipboardReadReplyEchoesOnlySelectionLetters)
{
    Term t(10, 2);
    t.feed("\x1b]52;c\x7f\xc3\xa9;?\x07");
    EXPECT_EQ(t.host.written, "\x1b]52;c;\x07");
    t.host.written.clear();
    t.feed("\x1b]52;\x7f;?\x07");
    EXPECT_EQ(t.host.written, "\x1b]52;c;\x07");
}

TEST(Terminal, NotificationBodiesAreBounded)
{
    Term t(10, 2);
    t.feed("\x1b]9;" + std::string(70000, 'x') + "\x07");
    t.feed("\x1b]777;notify;t;" + std::string(70000, 'x') + "\x07");
    EXPECT_EQ(t.host.notified, "");
}

TEST(Terminal, OscNumberMustBeWhole)
{
    Term t(10, 2);
    t.feed("\x1b]2x;bad\x07");
    EXPECT_EQ(t.host.title, "");
}

TEST(Terminal, DecimalValuesIgnoreTheLocale)
{
    // A comma-decimal locale must not change how rgbi: and opacities read.
    const char* locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8"};
    const std::string previous = std::setlocale(LC_NUMERIC, nullptr);
    bool commaLocale = false;
    for (const char* name : locales) {
        if (std::setlocale(LC_NUMERIC, name)) {
            commaLocale = true;
            break;
        }
    }
    if (!commaLocale) {
        GTEST_SKIP() << "no de_DE or fr_FR locale is installed: the C locale proves nothing";
    }
    Term t(10, 2);
    t.feed("\x1b]4;1;rgbi:0.5/0.5/0.5\x07\x1b]4;1;?\x07");
    t.feed("\x1b]21;transparent_background_color1=#102030@0.25\x07\x1b]21;transparent_background_color1=?\x07");
    std::setlocale(LC_NUMERIC, previous.c_str());
    EXPECT_EQ(t.host.written, "\x1b]4;1;rgb:8080/8080/8080\x07\x1b]21;transparent_background_color1=rgb:10/20/30@0.25\x07");
}

TEST(Terminal, RepeatShowsTheCharacterAsDrawn)
{
    Term t(10, 2);
    t.feed("q\x1b(0\x1b[2b\x1b(B");
    EXPECT_EQ(rowText(t.t, 0), "qqq"); // printed before the switch: repeats as printed
}

TEST(Terminal, RepeatInInsertModeShiftsOnce)
{
    Term t(10, 3);
    t.feed("0123456789\x1b[H\x1b[4ha\x1b[3b");
    EXPECT_EQ(rowText(t.t, 0), "aaaa012345");
    // More than the line holds: the rest wraps as printing would.
    Term wrap(4, 3);
    wrap.feed("xy\x1b[H\x1b[4ha\x1b[5b\x1b[4l");
    EXPECT_EQ(rowText(wrap.t, 0), "aaaa");
    EXPECT_EQ(rowText(wrap.t, 1), "aa");
}

TEST(Terminal, SizedTextIgnoresCharsets)
{
    Term t(10, 2);
    t.feed("\x1b(0\x1b]66;s=1;q\x1b\\\x1b(B");
    EXPECT_EQ(rowText(t.t, 0), "q");
}

TEST(Terminal, SizedBlocksMakeTheirLinesSingleWidth)
{
    Term t(80, 4);
    t.feed("\x1b#6\x1b[1;38H\x1b]66;w=4;X\x1b\\");
    EXPECT_EQ(t.t.screen().row(0).lineSize, LineSize::Single);
}

TEST(Terminal, RectangleCopyClipsAndSkipsBlocks)
{
    Term t(10, 4);
    t.feed("\xe3\x81\x82\xe3\x82\x99\x1b[1;1;1;2;1;1;10;1$v"); // あ with a mark, copied to the last column
    EXPECT_FALSE(t.t.screen().row(0).at(9).hasGrapheme());
    EXPECT_EQ(t.t.screen().row(0).at(9).codepoint(), 0u);
    Term blocks(10, 4);
    blocks.feed("\x1b]66;s=2;X\x1b\\\x1b[1;1;2;2;1;1;5;1$v");
    EXPECT_FALSE(blocks.t.screen().row(0).at(4).isMulticell());
    EXPECT_EQ(blocks.t.screen().row(0).at(4).codepoint(), 0u);
    Term overlap(4, 4);
    overlap.feed("a\r\nb\r\nc\x1b[1;1;2;1;1;2;1;1$v"); // rows 1-2 down by one
    EXPECT_EQ(rowText(overlap.t, 0), "a");
    EXPECT_EQ(rowText(overlap.t, 1), "a");
    EXPECT_EQ(rowText(overlap.t, 2), "b");
}

TEST(Terminal, OutOfRangeColoursAreIgnored)
{
    Term t(10, 2);
    t.feed("\x1b[31m\x1b[38;5;300mA\x1b[38;2;256;0;0mB\x1b[38:2::1:2:300mC");
    const auto& s = t.t.screen();
    for (int x = 0; x < 3; ++x) {
        EXPECT_EQ(s.styles().get(s.row(0).at(x).styleId()).fg, Color::palette(1)) << x;
    }
}

TEST(Terminal, ResetClearsKeyboardStacksAndImagesOfBothScreens)
{
    Term t(10, 3);
    t.t.resize(10, 3, 10, 20);
    t.feed("\x1b[>8u");
    t.feed(KittyPixel);
    t.feed("\x1b[?1049h\x1b[>8u");
    // A numbered image without a placement: entering the alternate screen
    // keeps it, so only the reset can clear it.
    t.feed("\x1b_Ga=t,i=9,f=24,s=1,v=1,q=2;AAAA\x1b\\");
    ASSERT_EQ(t.t.graphics().images().count(9), 1u);
    t.feed("\x1b" "c");
    t.host.written.clear();
    t.feed("\x1b[?u\x1b[?1049h\x1b[?u");
    EXPECT_TRUE(t.t.graphics().images().empty());
    t.feed("\x1b[?1049l");
    EXPECT_TRUE(t.t.graphics().images().empty());
    EXPECT_EQ(t.host.written, "\x1b[?0u\x1b[?0u");
}

TEST(Terminal, BellsRingOnlyForBel)
{
    Term t(10, 2);
    t.feed("\x07");
    EXPECT_EQ(t.host.bells, 1);
    t.feed("\x1b]2;t\x07"); // a BEL that ends a string
    EXPECT_EQ(t.host.bells, 1);
    t.feed("\x1b]1337;RequestAttention=yes\x07");
    EXPECT_EQ(t.host.bells, 2);
    t.feed("\x1b]1337;RequestAttention=no\x07");
    EXPECT_EQ(t.host.bells, 2);
}

TEST(Terminal, KittyKeyboardStackLimitsAndModes)
{
    Term t(10, 2);
    t.feed("\x1b[>1u");
    for (int i = 0; i < 64; ++i) {
        t.feed("\x1b[>2u");
    }
    t.feed("\x1b[<64u\x1b[?u"); // the first entry fell off the 64-deep stack
    EXPECT_EQ(t.host.written, "\x1b[?0u");
    t.host.written.clear();
    t.feed("\x1b[=15u\x1b[=4;3u\x1b[?u"); // mode 3 clears bits
    EXPECT_EQ(t.host.written, "\x1b[?11u");
}

TEST(Terminal, GraphemeClustersAreBounded)
{
    Term t(10, 2);
    std::string marks = "e";
    for (int i = 0; i < 40; ++i) {
        marks += "\xcc\x81";
    }
    t.feed(marks);
    EXPECT_EQ(t.t.screen().graphemeExtra(t.t.screen().row(0), 0).size(), 32u);
    EXPECT_EQ(t.t.screen().cursor.x, 1);
}

TEST(Terminal, ResetKeepsShellContexts)
{
    // The uapi context spec: a reset sequence leaves the context stack alone.
    Term t(10, 2);
    t.feed("\x1b]3008;start=a;type=shell\x1b\\");
    ASSERT_EQ(t.t.contexts().stack().size(), 1u);
    const int changes = t.host.contextChanges;
    t.feed("\x1b" "c");
    EXPECT_EQ(t.t.contexts().stack().size(), 1u);
    EXPECT_EQ(t.host.contextChanges, changes);
}
