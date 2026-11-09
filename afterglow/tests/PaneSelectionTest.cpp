// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Pane.h"

#include <gtest/gtest.h>

#include <string>

using Afterglow::Pane;
using AfterglowVt::SelectionMode;

namespace {

/// The cells that a press inside the selection finds (Pane::selectionContains),
/// against the selection as it is drawn.
class PaneSelection : public ::testing::Test {
protected:
    void feed(const std::string& bytes)
    {
        pane.withTerminal([&](AfterglowVt::Terminal& terminal) { terminal.feed(bytes); });
    }
    void select(int fromCol, int fromRow, int toCol, int toRow, SelectionMode mode = SelectionMode::Normal)
    {
        pane.startSelection(pane.selectionPointAt(fromCol, fromRow), mode, true);
        pane.extendSelection(pane.selectionPointAt(toCol, toRow));
    }

    Pane pane { AfterglowVt::TerminalOptions {} };
};

TEST_F(PaneSelection, AWordOrALineHoldsTheCellsThatItExpandsTo)
{
    feed("hello world");
    pane.startSelection(pane.selectionPointAt(1, 0), SelectionMode::Word, true);
    EXPECT_TRUE(pane.selectionContains(0, 0));
    EXPECT_TRUE(pane.selectionContains(4, 0));
    EXPECT_FALSE(pane.selectionContains(5, 0));
    pane.startSelection(pane.selectionPointAt(1, 0), SelectionMode::Line, true);
    EXPECT_TRUE(pane.selectionContains(pane.cols() - 1, 0));
    EXPECT_FALSE(pane.selectionContains(0, 1));
}

TEST_F(PaneSelection, AWideCharacterIsSelectedWhole)
{
    // The wide character takes columns 2 and 3: a selection that starts on
    // its right half holds its left half too.
    feed("ab\xe6\xbc\xa2" "cd");
    select(3, 0, 4, 0);
    EXPECT_TRUE(pane.selectionContains(2, 0));
    EXPECT_TRUE(pane.selectionContains(4, 0));
    EXPECT_FALSE(pane.selectionContains(1, 0));
}

TEST_F(PaneSelection, ABlockHoldsItsColumnsOnly)
{
    feed("abcdef\r\nabcdef\r\nabcdef");
    select(1, 0, 3, 2, SelectionMode::Block);
    EXPECT_TRUE(pane.selectionContains(2, 1));
    EXPECT_FALSE(pane.selectionContains(0, 1));
    EXPECT_FALSE(pane.selectionContains(5, 1));
    // A plain selection of the same cells runs on to the line ends.
    select(1, 0, 3, 2);
    EXPECT_TRUE(pane.selectionContains(5, 1));
}

TEST_F(PaneSelection, TheCellsAreThoseOfTheRowsShown)
{
    std::string lines;
    for (int i = 0; i < pane.rows() * 2; ++i) lines += "line " + std::to_string(i) + "\r\n";
    feed(lines);
    pane.withTerminal([](AfterglowVt::Terminal& terminal) { terminal.scrollViewport(5); });
    select(0, 0, 3, 0);
    EXPECT_TRUE(pane.selectionContains(1, 0));
    // The same cell of the view is another row once the view is back at the bottom.
    pane.withTerminal([](AfterglowVt::Terminal& terminal) { terminal.scrollViewport(-5); });
    EXPECT_FALSE(pane.selectionContains(1, 0));
}

TEST_F(PaneSelection, ASelectionOfTheOtherScreenHoldsNothing)
{
    feed("primary");
    select(0, 0, 6, 0);
    EXPECT_TRUE(pane.selectionContains(2, 0));
    feed("\x1b[?1049h");
    EXPECT_FALSE(pane.selectionContains(2, 0));
    feed("\x1b[?1049l");
    EXPECT_TRUE(pane.selectionContains(2, 0));
}

TEST_F(PaneSelection, TheCopyCursorIsNoSelection)
{
    feed("copy mode");
    pane.beginCopyMode();
    const int cursor = pane.withTerminal([](AfterglowVt::Terminal& terminal) { return terminal.screen().cursor.x; });
    EXPECT_FALSE(pane.selectionContains(cursor, 0));
    // A selection made in copy mode is one.
    pane.toggleCopySelection(SelectionMode::Normal);
    ASSERT_TRUE(pane.moveCopyCursor(-1, 0));
    EXPECT_TRUE(pane.selectionContains(cursor - 1, 0));
    pane.endCopyMode();
    // So is one that the mouse makes while copy mode runs.
    pane.beginCopyMode();
    select(0, 0, 3, 0);
    EXPECT_TRUE(pane.selectionContains(1, 0));
    pane.endCopyMode();
}

} // namespace
