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

TEST(Terminal, ShellIntegrationCommands)
{
    struct CommandHost : RecordingHost
    {
        std::vector<std::string> started;
        std::vector<int> finished;
        void commandStarted(std::string_view c) override { started.emplace_back(c); }
        void commandFinished(int code) override { finished.push_back(code); }
    };
    CommandHost host;
    Terminal t(20, 6, host);
    t.feed("\x1b]133;A\x07$ \x1b]133;B\x07make test\r\n\x1b]133;C\x07");
    t.feed("ok 1\r\nok 2\r\n\x1b]133;D;2\x07");
    ASSERT_EQ(host.started.size(), 1u);
    EXPECT_EQ(host.started[0], "make test");
    ASSERT_EQ(host.finished.size(), 1u);
    EXPECT_EQ(host.finished[0], 2);
    EXPECT_EQ(t.text(Terminal::TextScope::LastOutput), "ok 1\nok 2\n");
    // A new prompt without D finishes the command with an unknown code.
    t.feed("\x1b]133;A\x07$ \x1b]133;B\x07sleep 9\r\n\x1b]133;C\x07\x1b]133;A\x07");
    ASSERT_EQ(host.finished.size(), 2u);
    EXPECT_EQ(host.finished[1], -1);
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
    EXPECT_EQ(t.text(Terminal::TextScope::Screen), "xyyyyy\nyy");
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
    // UTF-8 runs, simple characters); fed a byte at a time it takes the
    // state machine and print(). The screens must come out the same.
    const std::string inputs[] = {
        "plain ascii that wraps past the edge of this narrow terminal\r\n",
        "Ελληνικά кириллица 日本語の文字 한국어 \xed\x95\x9c\xea\xb8\x80 wide at the edge: 中中中中中中中中中中中\r\n",
        "e\xcc\x81 n\xcc\x83 a\xcc\x8a\xcc\x8a combining after simple letters\r\n",
        "flags \xf0\x9f\x87\xaf\xf0\x9f\x87\xb5\xf0\x9f\x87\xba\xf0\x9f\x87\xb8 zwj \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9 vs16 \xe2\x9d\xa4\xef\xb8\x8f\r\n",
        "prepend \xd8\x80" "a then devanagari \xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7 hangul \xe1\x84\x80\xe1\x85\xa1\xe1\x86\xa8\r\n",
        "\x1b[31mred \xc3\xa9t\xc3\xa9\x1b[0m \x1b[1mbold \xce\xb1\xce\xb2\x1b[m\r\n",
        "bad \xc3 \xe2\x82 \xed\xa0\x80 \xc0\xaf \xf4\x90\x80\x80 \xff end\r\n",
        "\x1b[4h insert \xc3\xa9\xc3\xa9 \x1b[4l \x1b(0lqqk\x1b(B mixed\r\n",
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
            }
        }
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
}
