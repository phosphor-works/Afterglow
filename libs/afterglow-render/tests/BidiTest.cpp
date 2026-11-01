// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <AfterglowRender/Snapshot.h>
#include <gtest/gtest.h>
using namespace AfterglowRender;
using namespace AfterglowVt;
namespace {
struct Host : ITerminalHost { void writeToPty(std::string_view) override {} };
}
TEST(Bidi, ReordersWholeGraphemesAndPreservesLogicalQueries)
{
    Host host;
    TerminalOptions options;
    options.bidi = BidiMode::Auto;
    Terminal t(10, 2, host, options);
    t.feed("אבג");
    PaneSnapshot snap;
    snap.capture(t);
    EXPECT_EQ(snap.at(9, 0).cp, U'א');
    EXPECT_EQ(snap.at(8, 0).cp, U'ב');
    EXPECT_EQ(snap.at(7, 0).cp, U'ג');
    EXPECT_EQ(snap.logicalAt(0, 0).cp, U'א');
    EXPECT_EQ(snap.cursorX, snap.visualColumn(t.screen().cursor.x, 0));
    for (int col = 0; col < 10; ++col) EXPECT_EQ(snap.logicalColumn(snap.visualColumn(col, 0), 0), col);
    EXPECT_EQ(t.text(Terminal::TextScope::Screen).substr(0, 6), "אבג");
}
TEST(Bidi, WideCellsAndCombiningMarksStayTogether)
{
    Host host; TerminalOptions options; options.bidi = BidiMode::RightToLeft;
    Terminal t(12, 2, host, options);
    t.feed("א\xd6\xb0界ב");
    PaneSnapshot snap; snap.capture(t);
    const int wide = snap.visualColumn(1, 0);
    EXPECT_EQ(snap.at(wide, 0).cp, U'界');
    EXPECT_EQ(snap.at(wide + 1, 0).width, 0);
    EXPECT_EQ(snap.visualColumn(2, 0), wide + 1);
    const auto& alef = snap.logicalAt(0, 0);
    ASSERT_GT(alef.grapheme, 0u);
    EXPECT_EQ(snap.graphemes[alef.grapheme - 1], U"\u05b0");
}
TEST(Bidi, UsesParagraphContextAcrossSoftWrappedRows)
{
    Host host; TerminalOptions options; options.bidi = BidiMode::Auto;
    Terminal t(4, 3, host, options);
    t.feed("אבג 12");
    PaneSnapshot snap; snap.capture(t);
    EXPECT_EQ(snap.logicalAt(0, 1).cp, U'1');
    EXPECT_EQ(snap.logicalAt(0, 1).bidiLevel, 2);
    EXPECT_LT(snap.visualColumn(0, 1), snap.visualColumn(1, 1));
}
TEST(Bidi, ApplicationScreensAndDefaultModePreserveTheGrid)
{
    Host host; TerminalOptions options;
    Terminal off(8, 2, host, options); off.feed("אבג");
    PaneSnapshot snap; snap.capture(off);
    EXPECT_TRUE(snap.logicalToVisual.empty());
    options.bidi = BidiMode::Auto;
    Terminal t(8, 2, host, options);
    t.feed("\x1b[?1049hאבג"); snap.capture(t);
    EXPECT_TRUE(snap.logicalToVisual.empty());
    t.feed("\x1b[?1049l\x1b[?1000hאבג"); snap.capture(t);
    EXPECT_TRUE(snap.logicalToVisual.empty());
}

TEST(SnapshotCursor, ResolvesExtraCursorColorsPerCellAndTracksPalette)
{
    Host host; Terminal t(8,2,host);
    t.feed("\x1b[38;2;10;20;30m\x1b[48;2;40;50;60mA\x1b[>1;2:1:1 q");
    PaneSnapshot s; s.capture(t);
    ASSERT_EQ(s.extraCursors.size(),1u);
    EXPECT_EQ(s.extraCursors[0].color,s.cursorColor);
    EXPECT_EQ(s.extraCursors[0].textColor,s.cursorTextColor);
    t.feed("\x1b[>40;2:70:80:90 q\x1b[>30;2:100:110:120 q"); s.capture(t);
    EXPECT_EQ(s.extraCursors[0].color,packRgba(70,80,90));
    EXPECT_EQ(s.extraCursors[0].textColor,packRgba(100,110,120));
    t.feed("\x1b[>40;1 q"); s.capture(t);
    EXPECT_EQ(s.extraCursors[0].color,packRgba(10,20,30));
    EXPECT_EQ(s.extraCursors[0].textColor,packRgba(40,50,60));
    t.feed("\x1b[>40;5:1 q\x1b]4;1;rgb:12/34/56\x1b\\"); s.capture(t);
    EXPECT_EQ(s.extraCursors[0].color,packRgba(0x12,0x34,0x56));
}
