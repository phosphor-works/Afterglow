// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

#include <AfterglowVt/Selection.h>

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
    Selection select(int r0, int c0, int r1, int c1, SelectionMode mode = SelectionMode::Normal)
    {
        const Screen& s = t.screen();
        const std::uint64_t base = s.rowsDropped() + s.historySize();
        Selection sel;
        sel.anchor = {base + std::uint64_t(r0), c0};
        sel.head = {base + std::uint64_t(r1), c1};
        sel.mode = mode;
        sel.layoutEpoch = s.layoutEpoch();
        sel.active = true;
        return sel;
    }
};
} // namespace

TEST(Selection, StreamAcrossLines)
{
    Term t(10, 3);
    t.t.feed("hello\r\nworld");
    EXPECT_EQ(t.select(0, 1, 1, 2).text(t.t.screen()), "ello\nwor");
    // backwards drag gives the same text
    EXPECT_EQ(t.select(1, 2, 0, 1).text(t.t.screen()), "ello\nwor");
}

TEST(Selection, SoftWrapJoinsWithoutNewline)
{
    Term t(5, 3);
    t.t.feed("abcdefgh");
    EXPECT_EQ(t.select(0, 0, 1, 4).text(t.t.screen()), "abcdefgh");
}

TEST(Selection, WordAndLine)
{
    Term t(20, 2);
    t.t.feed("ls -la /usr/lib(x)");
    EXPECT_EQ(t.select(0, 8, 0, 8, SelectionMode::Word).text(t.t.screen()), "/usr/lib");
    EXPECT_EQ(t.select(0, 3, 0, 3, SelectionMode::Line).text(t.t.screen()), "ls -la /usr/lib(x)");
}

TEST(Selection, WordAcrossWrap)
{
    Term t(5, 3);
    t.t.feed("ab cdefgh ij");
    EXPECT_EQ(t.select(1, 1, 1, 1, SelectionMode::Word).text(t.t.screen()), "cdefgh");
}

TEST(Selection, Block)
{
    Term t(10, 3);
    t.t.feed("abcdef\r\nghijkl\r\nmnopqr");
    EXPECT_EQ(t.select(0, 1, 2, 3, SelectionMode::Block).text(t.t.screen()), "bcd\nhij\nnop");
}

TEST(Selection, WideCharacterSelectedWhole)
{
    Term t(10, 2);
    t.t.feed("a\xE4\xB8\xAD" "b");
    EXPECT_EQ(t.select(0, 0, 0, 1).text(t.t.screen()), "a\xE4\xB8\xAD");
}

TEST(Selection, SurvivesScrollingAndExpiresWithHistory)
{
    Term t(10, 2, 2);
    t.t.feed("one\r\ntwo");
    Selection sel = t.select(0, 0, 0, 2);
    t.t.feed("\r\nthree");
    EXPECT_EQ(sel.text(t.t.screen()), "one"); // now in history, same text
    t.t.feed("\r\nfour\r\nfive\r\nsix");
    EXPECT_FALSE(sel.validFor(t.t.screen())); // trimmed away
}

TEST(Selection, ResizeInvalidates)
{
    Term t(10, 2);
    t.t.feed("hello");
    Selection sel = t.select(0, 0, 0, 4);
    t.t.resize(8, 2, 8, 16);
    EXPECT_FALSE(sel.validFor(t.t.screen()));
}
