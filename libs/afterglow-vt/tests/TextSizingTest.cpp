// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowVt/Selection.h>

#include <gtest/gtest.h>

#include <string>

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
    void feed(std::string_view s) { t.feed(s); }
    const Cell& at(int x, int y) const { return t.screen().row(y).at(x); }
};
} // namespace

TEST(TextSizing, ScaledCharactersCoverBlocks)
{
    Term t(20, 5);
    t.feed("\x1b]66;s=2;ab\x07"); // two 2x2 blocks
    EXPECT_EQ(t.t.screen().cursor.x, 4);
    EXPECT_EQ(t.t.screen().cursor.y, 0);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            const Cell& c = t.at(x, y);
            ASSERT_TRUE(c.isMulticell()) << x << "," << y;
            EXPECT_EQ(c.multicellDx(), x % 2);
            EXPECT_EQ(c.multicellDy(), y);
            EXPECT_EQ(c.codepoint(), x < 2 ? U'a' : U'b');
        }
    }
    const MulticellParams* p = t.t.screen().multicellParams(t.t.screen().row(1), 3);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->scale, 2);
    EXPECT_EQ(p->columns(), 2);
    EXPECT_FALSE(t.at(4, 0).isMulticell());
}

TEST(TextSizing, ExplicitWidthHoldsTheWholeText)
{
    Term t(20, 5);
    t.feed("\x1b]66;w=2;\xf0\x9f\x90\x88\x07x"); // a cat in two cells, then x
    EXPECT_TRUE(t.at(0, 0).isMulticell());
    EXPECT_TRUE(t.at(1, 0).isMulticell());
    EXPECT_EQ(t.at(2, 0).codepoint(), U'x');
    // Superscript: two characters in one cell at half size.
    t.feed("\x1b]66;n=1:d=2:w=1;12\x07");
    EXPECT_EQ(t.at(3, 0).codepoint(), U'1');
    EXPECT_EQ(t.t.screen().graphemeExtra(t.t.screen().row(0), 3), U"2");
    EXPECT_EQ(t.t.screen().cursor.x, 4);
}

TEST(TextSizing, TextIsCopiedOnce)
{
    Term t(20, 5);
    t.feed("x\x1b]66;s=3;Hi\x07y");
    const Screen& s = t.t.screen();
    const std::uint64_t base = s.rowsDropped() + s.historySize();
    Selection sel;
    sel.anchor = {base, 0};
    sel.head = {base, 19};
    sel.layoutEpoch = s.layoutEpoch();
    sel.active = true;
    EXPECT_EQ(sel.text(s), "xHiy");
}

TEST(TextSizing, OverwriteRules)
{
    Term t(20, 5);
    t.feed("\x1b]66;s=2;ab\x07");
    t.feed("\x1b[1;1HZ"); // the top-left cell: the block goes
    EXPECT_EQ(rowText(t.t, 0), "Z b"); // "a" erased to blanks, "b" remains as a block
    EXPECT_FALSE(t.at(1, 1).isMulticell());
    t.feed("\x1b[1;4HQ"); // the top row, not top-left: the block becomes spaces
    EXPECT_EQ(rowText(t.t, 0), "Z  Q");
    EXPECT_FALSE(t.at(2, 1).isMulticell());

    Term u(20, 5);
    u.feed("\x1b]66;s=2;a\x07\x1b[2;1HXY"); // a lower row: text goes after the block
    EXPECT_TRUE(u.at(0, 1).isMulticell());
    EXPECT_EQ(u.at(2, 1).codepoint(), U'X');
    EXPECT_EQ(u.at(3, 1).codepoint(), U'Y');
}

TEST(TextSizing, EraseAndEditTakeWholeBlocks)
{
    Term t(20, 5);
    t.feed("\x1b]66;s=2;a\x07");
    t.feed("\x1b[2;2H\x1b[K"); // erasing part of the lower row erases the block
    for (int y = 0; y < 2; ++y) {
        EXPECT_FALSE(t.at(0, y).isMulticell());
        EXPECT_FALSE(t.at(1, y).isMulticell());
    }
    t.feed("\x1b[1;1H\x1b]66;s=2;a\x07");
    ASSERT_TRUE(t.at(0, 1).isMulticell()); // the block is back before the DCH
    t.feed("\x1b[1;1H\x1b[P"); // DCH through it
    EXPECT_FALSE(t.at(0, 1).isMulticell());
}

TEST(TextSizing, AScrollWithinMarginsKeepsTheParametersOfTheBlocksItMoves)
{
    // A block that needs a row below the region scrolls the region within
    // the left and right margins. The rows that the scroll leaves held a
    // copy of the blocks that moved: clearing them took the moved block's
    // parameters, and text printed on the block then never moved past it.
    Term t(20, 5);
    t.feed("\x1b[?69h\x1b[2;6s");
    t.feed("\x1b[4;4H\x1b]66;s=2;a\x07"); // rows 3-4, columns 3-4
    t.feed("\x1b[5;2H\x1b]66;s=2;b\x07"); // scrolls a up to rows 2-3
    const Screen& s = t.t.screen();
    ASSERT_TRUE(t.at(3, 3).isMulticell());
    EXPECT_EQ(t.at(3, 3).multicellDy(), 1);
    EXPECT_NE(s.multicellParams(s.row(2), 3), nullptr);
    EXPECT_NE(s.multicellParams(s.row(3), 3), nullptr);
    EXPECT_NE(s.multicellParams(s.row(3), 1), nullptr);
    // Text on the lower row of a goes past it.
    t.feed("\x1b[4;5Hx");
    EXPECT_NE(t.t.text(Terminal::TextScope::Screen).find('x'), std::string::npos) << t.t.text(Terminal::TextScope::Screen);
}

TEST(TextSizing, AScrollDownWithinMarginsKeepsTheParametersOfTheBlocksItMoves)
{
    // A scroll down (SD, and a reverse index at the top margin) leaves the
    // top row, which held a copy of the block's top row. The reverse index
    // is within the left and right margins: outside them it scrolls nothing.
    for (const std::string scroll : {"\x1b[T", "\x1b[1;2H\x1bM"}) {
        SCOPED_TRACE(scroll);
        Term t(20, 5);
        t.feed("\x1b[?69h\x1b[2;6s");
        t.feed("\x1b[1;3H\x1b]66;s=2;a\x07"); // rows 0-1, columns 2-3
        t.feed(scroll);                         // to rows 1-2
        const Screen& s = t.t.screen();
        EXPECT_FALSE(t.at(2, 0).isMulticell());
        ASSERT_TRUE(t.at(2, 1).isMulticell());
        ASSERT_TRUE(t.at(2, 2).isMulticell());
        EXPECT_EQ(t.at(2, 1).multicellDy(), 0);
        EXPECT_EQ(t.at(2, 2).multicellDy(), 1);
        EXPECT_NE(s.multicellParams(s.row(1), 2), nullptr);
        EXPECT_NE(s.multicellParams(s.row(2), 2), nullptr);
        t.feed("\x1b[3;4Hx");
        EXPECT_NE(t.t.text(Terminal::TextScope::Screen).find('x'), std::string::npos) << t.t.text(Terminal::TextScope::Screen);
    }
}

TEST(TextSizing, AScrollOfSeveralRowsWithinMarginsKeepsTheBlocksItMoves)
{
    // Two rows up: the block goes from rows 2-4 to rows 0-2. Rows 3 and 4,
    // which the scroll clears, keep the remnant of its lower rows: erased as
    // a block, it would reach up into row 2, which holds its bottom row now.
    Term t(20, 5);
    t.feed("\x1b[?69h\x1b[2;6s");
    t.feed("\x1b[3;3H\x1b]66;s=3;a\x07"); // rows 2-4, columns 2-4
    t.feed("\x1b[2S");                     // to rows 0-2
    const Screen& s = t.t.screen();
    for (int y = 0; y < 3; ++y) {
        ASSERT_TRUE(t.at(2, y).isMulticell()) << y;
        EXPECT_EQ(t.at(2, y).multicellDy(), y);
        EXPECT_NE(s.multicellParams(s.row(y), 2), nullptr) << y;
    }
    EXPECT_FALSE(t.at(2, 3).isMulticell());
    EXPECT_FALSE(t.at(2, 4).isMulticell());
    t.feed("\x1b[3;4Hx");
    EXPECT_NE(t.t.text(Terminal::TextScope::Screen).find('x'), std::string::npos) << t.t.text(Terminal::TextScope::Screen);
}

TEST(TextSizing, TextOnABlockThatAMarginScrollMovedGoesOn)
{
    // The sequence that a random run found: an image that moves the cursor
    // down, text that wraps within the margins, then blocks at the bottom.
    Term t(47, 5);
    t.t.resize(47, 5, 10, 20);
    std::string pixels;
    for (int i = 0; i < 40; ++i) pixels += std::string("\x01\x02\x03\xff", 4);
    t.feed("\x1b[?69h\x1b[2;6s");
    t.feed("\x1b_Ga=T,f=32,s=1,v=40,i=1,q=2;" + AfterglowProto::base64Encode(pixels) + "\x1b\\");
    t.feed("\rhello world \x1b]66;s=2;ab\x1b\\x");
    EXPECT_NE(t.t.text(Terminal::TextScope::Screen).find('x'), std::string::npos) << t.t.text(Terminal::TextScope::Screen);
}

TEST(TextSizing, EraseCharactersAndDisplayTakeWholeBlocks)
{
    // kitty 0.50.0 fixed erasing multi-cell characters. Each erase that
    // reaches one cell of a block, from any row or column, erases all of it.
    Term t(20, 5);
    auto blockAt = [&](int x) { return t.at(x, 0).isMulticell() || t.at(x, 1).isMulticell(); };
    t.feed("\x1b]66;s=2;ab\x07"); // blocks at columns 0-1 and 2-3, rows 0-1
    t.feed("\x1b[2;4H\x1b[X"); // ECH on the last cell of the lower row of b
    EXPECT_TRUE(blockAt(0));
    EXPECT_TRUE(blockAt(1));
    for (int y = 0; y < 2; ++y) {
        EXPECT_FALSE(t.at(2, y).isMulticell()) << y;
        EXPECT_FALSE(t.at(3, y).isMulticell()) << y;
    }
    t.feed("\x1b[2;2H\x1b[1J"); // ED 1 ends in the lower row of a
    for (int y = 0; y < 2; ++y) {
        EXPECT_FALSE(t.at(0, y).isMulticell()) << y;
        EXPECT_FALSE(t.at(1, y).isMulticell()) << y;
    }
    t.feed("\x1b[H\x1b]66;s=2;c\x07\x1b[2;2H\x1b[J"); // ED 0 starts in the lower row
    for (int y = 0; y < 2; ++y) {
        EXPECT_FALSE(t.at(0, y).isMulticell()) << y;
        EXPECT_FALSE(t.at(1, y).isMulticell()) << y;
    }
    t.feed("\x1b[H\x1b]66;s=2;d\x07\x1b[1;2H\x1b[1K"); // EL 1 ends in the right column
    for (int y = 0; y < 2; ++y) {
        EXPECT_FALSE(t.at(0, y).isMulticell()) << y;
        EXPECT_FALSE(t.at(1, y).isMulticell()) << y;
    }
}

TEST(TextSizing, AVariationSelectorKeepsAnExplicitWidth)
{
    // kitty 0.49.0: U+FE0E after a cell with an explicit width does not
    // narrow it, as it narrows an emoji of natural width.
    Term t(20, 5);
    t.feed("\x1b]66;w=2;\xe2\x9d\xa4\x07\xef\xb8\x8ex"); // a heart in two cells, U+FE0E, x
    EXPECT_TRUE(t.at(0, 0).isMulticell());
    EXPECT_TRUE(t.at(1, 0).isMulticell());
    const MulticellParams* p = t.t.screen().multicellParams(t.t.screen().row(0), 0);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->columns(), 2);
    EXPECT_EQ(t.at(2, 0).codepoint(), U'x');
}

TEST(TextSizing, ALongClusterOfNaturalWidthStaysWhole)
{
    // kitty 0.49.0 overflowed a buffer with natural-width clusters of more
    // than four code points. A family emoji is seven.
    const std::string family = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7"
                               "\xe2\x80\x8d\xf0\x9f\x91\xa6";
    Term t(20, 5);
    t.feed("\x1b]66;s=2;" + family + "\x07x");
    EXPECT_EQ(t.t.text(Terminal::TextScope::Screen), family + "x");
    EXPECT_EQ(t.t.screen().cursor.x, 5); // two columns at scale 2, then x
    // Combining marks: one base and twelve marks.
    std::string marks = "e";
    for (int i = 0; i < 12; ++i) marks += "\xcc\x81";
    t.t.reset();
    t.feed("\x1b]66;s=2;" + marks + "\x07");
    EXPECT_EQ(t.t.text(Terminal::TextScope::Screen), marks);
    EXPECT_EQ(t.t.screen().cursor.x, 2);
}

TEST(TextSizing, ACodeWithNoTextChangesNothing)
{
    // As in kitty: no text, whatever the metadata, draws nothing and moves
    // no cursor. afterglow_vtdiff found that Afterglow drew a space.
    for (std::string_view code : {"\x1b]66;;\x07", "\x1b]66;w=2;\x07", "\x1b]66;s=2;\x07", "\x1b]66;w=3:s=2;\x1b\\",
                                  "\x1b]66;w=2;\x01\x02\x07"}) {
        Term t(20, 5);
        t.feed("ab");
        t.feed(code);
        t.feed("cd");
        EXPECT_EQ(rowText(t.t, 0), "abcd") << code;
        EXPECT_EQ(t.t.screen().cursor.x, 4) << code;
        EXPECT_FALSE(t.at(2, 0).isMulticell()) << code;
    }
}

TEST(TextSizing, WrapsAndScrollsToFit)
{
    Term t(5, 3);
    t.feed("abcd\x1b]66;s=2;X\x07"); // no room on the line: next line
    EXPECT_TRUE(t.at(0, 1).isMulticell());
    EXPECT_TRUE(t.at(1, 2).isMulticell());
    Term u(5, 3);
    u.feed("\x1b[3;1H\x1b]66;s=2;Y\x07"); // the last row: the screen scrolls up
    EXPECT_TRUE(u.at(0, 1).isMulticell());
    EXPECT_EQ(u.at(0, 1).multicellDy(), 0);
    EXPECT_EQ(u.at(0, 2).multicellDy(), 1);
    Term w(4, 3);
    w.feed("\x1b]66;s=5;Z\x07"); // larger than the screen: discarded
    EXPECT_EQ(rowText(w.t, 0), "");
}

TEST(TextSizing, BlocksScrollAsAUnitAndRegionsCutThem)
{
    Term t(10, 6);
    t.feed("\x1b[3;1H\x1b]66;s=2;Q\x07\x1b[6;1H\n");
    EXPECT_TRUE(t.at(0, 1).isMulticell());
    EXPECT_EQ(t.at(0, 1).multicellDy(), 0);
    EXPECT_EQ(t.at(0, 2).multicellDy(), 1);
    // A scroll region through the middle of the block erases it.
    t.feed("\x1b[3;6r\x1b[6;1H\n");
    EXPECT_FALSE(t.at(0, 1).isMulticell());
}

TEST(TextSizing, ReflowFlattensBlocks)
{
    Term t(10, 4);
    t.feed("\x1b]66;s=2;W\x07");
    t.t.resize(8, 4, 8, 16);
    EXPECT_FALSE(t.at(0, 0).isMulticell());
    EXPECT_EQ(rowText(t.t, 0), "W");
}

TEST(TextSizing, ACombiningMarkJoinsTheBlockItFollows)
{
    // kitty: "If the text is a combining character it is added to the
    // existing multicell character". Every row of the block keeps its text.
    Term t(20, 5);
    t.feed("\x1b]66;s=2;a\x07\xcc\x81"); // a 2x2 'a', then U+0301
    for (int y = 0; y < 2; ++y) {
        EXPECT_EQ(t.t.screen().graphemeExtra(t.t.screen().row(y), 0), std::u32string_view(U"\u0301")) << y;
        EXPECT_FALSE(t.at(1, y).hasGrapheme()) << y;
    }
    EXPECT_EQ(t.t.screen().cursor.x, 2);
}

TEST(TextSizing, PlainSizeIsOrdinaryText)
{
    Term t(10, 2);
    t.feed("\x1b]66;s=1;plain\x07");
    EXPECT_EQ(rowText(t.t, 0), "plain");
    EXPECT_FALSE(t.at(0, 0).isMulticell());
}

TEST(LineSize, DoubleWidthHalvesTheLine)
{
    Term t(10, 3);
    t.feed("abcdefghij\x1b[1;1H\x1b#6");
    EXPECT_EQ(t.t.screen().row(0).lineSize, LineSize::DoubleWidth);
    t.feed("\x1b[1;9H");
    EXPECT_EQ(t.t.screen().cursor.x, 4); // the right half is out of reach
    t.feed("XYZ"); // X fills the half; the rest wraps
    EXPECT_EQ(t.at(4, 0).codepoint(), U'X');
    EXPECT_EQ(rowText(t.t, 1), "YZ");
    t.feed("\x1b[1;1H\x1b#5"); // single width again: the hidden half comes back
    EXPECT_EQ(rowText(t.t, 0), "abcdXfghij");
}

TEST(LineSize, DoubleHeightAndResets)
{
    Term t(10, 3);
    t.feed("\x1b#3\r\n\x1b#4");
    EXPECT_EQ(t.t.screen().row(0).lineSize, LineSize::DoubleTop);
    EXPECT_EQ(t.t.screen().row(1).lineSize, LineSize::DoubleBottom);
    t.feed("\x1b[2J");
    EXPECT_EQ(t.t.screen().row(0).lineSize, LineSize::Single);
    t.feed("\x1b#6\x1b#8"); // DECALN
    EXPECT_EQ(t.t.screen().row(0).lineSize, LineSize::Single);
}

TEST(TextSizing, ExplicitBlockPreservesLongTextAndRejectsOverlongUtf8)
{
    Term t(20, 3);
    const std::string text(200, 'a');
    t.feed("\x1b]66;w=2;" + text + "\a");
    EXPECT_EQ(t.t.text(Terminal::TextScope::Screen), text);
    t.t.reset();
    t.feed("\x1b]66;w=2;\xc1\xa1\a");
    EXPECT_EQ(t.at(0, 0).codepoint(), Unicode::ReplacementCharacter);
}

TEST(TextSizing, IllFormedBytesBecomeReplacementsAndControlsGo)
{
    Term t(20, 3);
    // A cut sequence (E2 82) gives one U+FFFD for each byte. An encoded C1
    // control (U+0085) and a DEL are dropped.
    t.feed("\x1b]66;s=1;a\xe2\x82\xc2\x85\x7f" "b\x07");
    EXPECT_EQ(t.at(0, 0).codepoint(), U'a');
    EXPECT_EQ(t.at(1, 0).codepoint(), Unicode::ReplacementCharacter);
    EXPECT_EQ(t.at(2, 0).codepoint(), Unicode::ReplacementCharacter);
    EXPECT_EQ(t.at(3, 0).codepoint(), U'b');
    EXPECT_EQ(t.t.screen().cursor.x, 4);
}

TEST(TextSizing, MetadataAndTextAreBounded)
{
    Term t(20, 3);
    t.feed("\x1b]66;w=2;" + std::string(4097, 'a') + "\x07"); // text over 4,096 bytes: dropped
    EXPECT_TRUE(t.at(0, 0).isEmpty());
    t.feed("\x1b]66;s=8;X\x07"); // scale out of range: plain text
    EXPECT_FALSE(t.at(0, 0).isMulticell());
    EXPECT_EQ(t.at(0, 0).codepoint(), U'X');
    t.feed("\x1b[2;1H\x1b]66;n=2:d=2:w=1;Y\x07"); // a fraction of one or more: full size
    ASSERT_TRUE(t.at(0, 1).isMulticell());
    const MulticellParams* params = t.t.screen().multicellParams(t.t.screen().row(1), 0);
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(params->num, 0);
    EXPECT_EQ(params->den, 0);
}
