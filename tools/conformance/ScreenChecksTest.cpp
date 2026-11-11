// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The checks of afterglow_tuifuzz: a screen that the terminal core made
// passes, and a screen broken on purpose fails with the right message.

#include "ScreenChecks.h"

#include <AfterglowVt/Terminal.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

using AfterglowConformance::checkScreen;
using AfterglowConformance::checkTerminal;
using AfterglowVt::Screen;

struct NullHost : AfterglowVt::ITerminalHost
{
    void writeToPty(std::string_view) override { }
};

/// The message of a failed check, or "" when the screen passed.
std::string failure(const Screen& s)
{
    return checkScreen(s, "screen").value_or("");
}

TEST(ScreenChecks, AScreenOfTheTerminalCorePasses)
{
    NullHost host;
    AfterglowVt::Terminal t(20, 6, host);
    // Wide characters, clusters, sized text, scrollback, the alternate
    // screen, and resizes that rewrap all of it.
    for (int i = 0; i < 12; ++i) {
        t.feed("中文 é e\xcc\x81 😀 \x1b]66;s=2;ab\x07 x\r\n");
    }
    t.feed("\x1b[?1049h中文\x1b]66;w=3;q\x07");
    EXPECT_EQ(checkTerminal(t).value_or(""), "");
    t.resize(7, 3, 8, 16);
    EXPECT_EQ(checkTerminal(t).value_or(""), "");
    t.feed("\x1b[?1049l");
    t.resize(33, 9, 8, 16);
    EXPECT_EQ(checkTerminal(t).value_or(""), "");
}

TEST(ScreenChecks, ACursorOutsideTheScreenFails)
{
    Screen s(10, 3, 10);
    s.cursor.x = 10;
    EXPECT_EQ(failure(s), "screen: cursor 10,0 outside 10x3");
}

TEST(ScreenChecks, ARowOfTheWrongWidthFails)
{
    Screen s(10, 3, 10);
    s.row(1).cells.resize(9);
    EXPECT_EQ(failure(s), "screen: row 1 has 9 cells, the screen 10");
}

TEST(ScreenChecks, AWideCharacterWithNoTailFails)
{
    Screen s(10, 3, 10);
    s.row(0).cells[2].setCodepoint(U'中');
    s.row(0).cells[2].setWidth(2);
    EXPECT_EQ(failure(s), "screen: row 0 column 2: a wide character with no tail cell");
}

TEST(ScreenChecks, AWideCharacterInTheLastColumnFails)
{
    Screen s(10, 3, 10);
    s.row(2).cells[9].setCodepoint(U'中');
    s.row(2).cells[9].setWidth(2);
    EXPECT_EQ(failure(s), "screen: row 2 column 9: a wide character in the last column");
}

TEST(ScreenChecks, ATailWithNoWideCharacterFails)
{
    Screen s(10, 3, 10);
    s.row(0).cells[5].setWidth(0);
    EXPECT_EQ(failure(s), "screen: row 0 column 5: a tail cell with no wide character");
}

TEST(ScreenChecks, ASizedBlockWithNoFirstCellFails)
{
    Screen s(10, 3, 10);
    s.row(1).cells[4].setMulticell(1, 0);
    EXPECT_EQ(failure(s), "screen: row 1 column 4: a sized block with no top-left cell");
}

TEST(ScreenChecks, ASizedBlockThatLostACellFails)
{
    // A block of scale 2: two columns, two rows. Its last cell is plain.
    Screen s(10, 3, 10);
    s.row(0).multicell = std::make_unique<AfterglowVt::MulticellMap>();
    AfterglowVt::MulticellParams params;
    params.scale = 2;
    (*s.row(0).multicell)[3] = params;
    s.row(0).cells[3].setCodepoint(U'a');
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            s.row(dy).cells[std::size_t(3 + dx)].setMulticell(dx, dy);
        }
    }
    EXPECT_EQ(failure(s), "");
    s.row(1).cells[4].setCodepoint(U'b');
    EXPECT_EQ(failure(s), "screen: row 0 column 3: a sized block that lost its cell 1,1");
}

TEST(ScreenChecks, ACellOutsideItsBlockFails)
{
    Screen s(10, 3, 10);
    s.row(0).multicell = std::make_unique<AfterglowVt::MulticellMap>();
    (*s.row(0).multicell)[0] = AfterglowVt::MulticellParams{}; // one cell
    s.row(0).cells[0].setMulticell(0, 0);
    s.row(0).cells[1].setMulticell(1, 0);
    EXPECT_EQ(failure(s), "screen: row 0 column 1: a cell outside the size of its block");
}

} // namespace
