// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

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
