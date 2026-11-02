// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DockArea.h"

#include <AfterglowVt/Style.h>

#include <gtest/gtest.h>

#include <memory>

using namespace Afterglow;

namespace {
constexpr int Cell = 10;

QVariantMap run(const QString& text, const QString& item = {})
{
    QVariantMap r{{QStringLiteral("text"), text}};
    if (!item.isEmpty()) r.insert(QStringLiteral("item"), item);
    return r;
}

QVariantMap spacer()
{
    return {{QStringLiteral("spacer"), true}};
}

/// A one-row dock @p cols cells wide holding one line of @p runs.
std::unique_ptr<DockArea> dock(const QVariantList& runs, int cols = 20)
{
    const QVariantList lines{QVariant(runs)};
    auto area = std::make_unique<DockArea>(QStringLiteral("owner@1"), QStringLiteral("bar"), AfterglowVt::TerminalOptions{});
    area->setContent({{QStringLiteral("edge"), QStringLiteral("top")}, {QStringLiteral("size"), 1},
                      {QStringLiteral("lines"), lines}}, 1);
    area->setRect(QRect(0, 0, cols * Cell, Cell), Cell, Cell);
    return area;
}

QString rowText(DockArea& area, int row = 0)
{
    const auto& snap = area.snapshot();
    QString text;
    for (int x = 0; x < snap.cols; ++x) {
        const auto& cell = snap.at(x, row);
        if (cell.width == 0) continue; // the tail of a wide character
        text += cell.cp ? QString::fromUcs4(&cell.cp, 1) : QStringLiteral(" ");
    }
    return text;
}

QString rowText(const std::unique_ptr<DockArea>& area, int row = 0)
{
    return rowText(*area, row);
}

QString itemAtColumn(const std::unique_ptr<DockArea>& area, int col)
{
    return area->itemAt(QPoint(col * Cell + Cell / 2, Cell / 2));
}
} // namespace

TEST(DockArea, SpacersPushTheRunsAfterThemToTheRightEdge)
{
    auto area = dock(QVariantList{run(QStringLiteral("left"), QStringLiteral("l")), spacer(),
                                   run(QStringLiteral("right"), QStringLiteral("r"))});
    EXPECT_EQ(rowText(area), QStringLiteral("left           right"));
    EXPECT_EQ(itemAtColumn(area, 0), QStringLiteral("l"));
    EXPECT_EQ(itemAtColumn(area, 10), QString());
    EXPECT_EQ(itemAtColumn(area, 15), QStringLiteral("r"));
    EXPECT_EQ(itemAtColumn(area, 19), QStringLiteral("r"));
}

TEST(DockArea, TwoSpacersShareTheFreeCellsAndCentreTheMiddle)
{
    auto area = dock(QVariantList{run(QStringLiteral("a")), spacer(), run(QStringLiteral("mid")), spacer(), run(QStringLiteral("b"))});
    // 15 free cells: the first spacer takes the odd one.
    EXPECT_EQ(rowText(area), QStringLiteral("a        mid       b"));
}

TEST(DockArea, WideTextCountsTwoCellsAndLongTextIsCutAtTheEdge)
{
    auto area = dock(QVariantList{run(QStringLiteral("日本"), QStringLiteral("jp")), spacer(), run(QStringLiteral("x"))}, 10);
    EXPECT_EQ(rowText(area).remove(QChar(' ')), QStringLiteral("日本x"));
    EXPECT_EQ(itemAtColumn(area, 3), QStringLiteral("jp"));
    EXPECT_EQ(itemAtColumn(area, 4), QString());
    auto cut = dock(QVariantList{run(QString(30, QLatin1Char('y')), QStringLiteral("long"))}, 10);
    EXPECT_EQ(rowText(cut), QString(10, QLatin1Char('y')));
    EXPECT_EQ(itemAtColumn(cut, 9), QStringLiteral("long"));
}

TEST(DockArea, ControlCharactersShowAsSpacesAndActOnNothing)
{
    // An escape sequence in the text must not reach the terminal as one.
    auto area = dock(QVariantList{run(QStringLiteral("a\x1b[2Jb\nc"))});
    EXPECT_EQ(rowText(area).trimmed(), QStringLiteral("a [2Jb c"));
}

TEST(DockArea, NewTextKeepsItsPlaceAndAMoveTakesANewOne)
{
    DockArea area(QStringLiteral("owner@1"), QStringLiteral("bar"), AfterglowVt::TerminalOptions{});
    const QVariantMap top{{QStringLiteral("edge"), QStringLiteral("top")}, {QStringLiteral("size"), 1}, {QStringLiteral("lines"), QVariantList{}}};
    EXPECT_EQ(area.setContent(top, 1), DockArea::Change::Place);
    QVariantMap text = top;
    text.insert(QStringLiteral("lines"), QVariantList{QVariantList{run(QStringLiteral("new"))}});
    EXPECT_EQ(area.setContent(text, 2), DockArea::Change::Content);
    EXPECT_EQ(area.order(), 1u);
    QVariantMap bottom = top;
    bottom.insert(QStringLiteral("edge"), QStringLiteral("bottom"));
    EXPECT_EQ(area.setContent(bottom, 3), DockArea::Change::Place);
    EXPECT_EQ(area.order(), 3u);
}

TEST(DockArea, TheSameEffectAgainChangesNothingAndDrawsNothing)
{
    auto area = dock(QVariantList{run(QStringLiteral("same"), QStringLiteral("s"))});
    const auto version = area->snapshot().version;
    const QVariantMap effect{{QStringLiteral("edge"), QStringLiteral("top")}, {QStringLiteral("size"), 1},
                             {QStringLiteral("lines"), QVariantList{QVariant(QVariantList{run(QStringLiteral("same"), QStringLiteral("s"))})}}};
    EXPECT_EQ(area->setContent(effect, 5), DockArea::Change::None);
    EXPECT_EQ(area->order(), 1u);
    // Not drawn again: the renderer's cache of the area stays valid.
    EXPECT_EQ(area->snapshot().version, version);
    QVariantMap filled = effect;
    filled.insert(QStringLiteral("fill"), 4);
    EXPECT_EQ(area->setContent(filled, 6), DockArea::Change::Content);
    EXPECT_NE(area->snapshot().version, version);
    area->setOptions(AfterglowVt::TerminalOptions{});
    const auto drawn = area->snapshot().version;
    area->setOptions(AfterglowVt::TerminalOptions{}); // the same options: nothing to draw
    EXPECT_EQ(area->snapshot().version, drawn);
}

TEST(DockArea, TextPastTheEdgeIsCutAndTheRunsAfterItTakeNoCells)
{
    // With autowrap off, the terminal itself would write every character
    // past the edge into the last cell: "abcdefghip".
    auto area = dock(QVariantList{run(QStringLiteral("abcdefghijklmnop"), QStringLiteral("long")),
                                  run(QStringLiteral("XYZ"), QStringLiteral("after"))}, 10);
    EXPECT_EQ(rowText(area), QStringLiteral("abcdefghij"));
    EXPECT_EQ(itemAtColumn(area, 9), QStringLiteral("long"));
    EXPECT_EQ(area->text(), QStringLiteral("abcdefghij"));
    // A spacer that gets no cells, then a run: the run is cut too.
    auto full = dock(QVariantList{run(QStringLiteral("0123456789")), spacer(), run(QStringLiteral("Q"), QStringLiteral("q"))}, 10);
    EXPECT_EQ(rowText(full), QStringLiteral("0123456789"));
    EXPECT_EQ(itemAtColumn(full, 9), QString());
}

TEST(DockArea, AWideCharacterThatDoesNotFitLeavesABlankCellNotAReplacement)
{
    auto straddles = dock(QVariantList{run(QStringLiteral("abc日"), QStringLiteral("t"))}, 4);
    EXPECT_EQ(rowText(straddles), QStringLiteral("abc "));
    // The blank cell is still the run's.
    EXPECT_EQ(itemAtColumn(straddles, 3), QStringLiteral("t"));
    auto after = dock(QVariantList{run(QStringLiteral("abcd")), run(QStringLiteral("日"))}, 4);
    EXPECT_EQ(rowText(after), QStringLiteral("abcd"));
    for (const auto& cell : after->snapshot().cells) EXPECT_NE(cell.cp, char32_t(0xFFFD));
    // A combining mark at the edge still joins the character before it.
    auto mark = dock(QVariantList{run(QStringLiteral("abce\u0301"))}, 4);
    EXPECT_EQ(rowText(mark), QStringLiteral("abce"));
    EXPECT_NE(mark->snapshot().at(3, 0).grapheme, 0u);
}

TEST(DockArea, RowsReadLeftToRightWhateverThePanesBidiMode)
{
    AfterglowVt::TerminalOptions options;
    options.bidi = AfterglowVt::BidiMode::RightToLeft;
    DockArea area(QStringLiteral("owner@1"), QStringLiteral("bar"), options);
    const QVariantList runs{run(QStringLiteral("שלום"), QStringLiteral("he")), spacer(), run(QStringLiteral("ok"), QStringLiteral("ok"))};
    area.setContent({{QStringLiteral("edge"), QStringLiteral("top")}, {QStringLiteral("size"), 1},
                     {QStringLiteral("lines"), QVariantList{QVariant(runs)}}}, 1);
    area.setRect(QRect(0, 0, 10 * Cell, Cell), Cell, Cell);
    EXPECT_EQ(rowText(area), QStringLiteral("שלום    ok"));
    EXPECT_EQ(area.itemAt(QPoint(Cell / 2, Cell / 2)), QStringLiteral("he"));
    EXPECT_EQ(area.itemAt(QPoint(9 * Cell + Cell / 2, Cell / 2)), QStringLiteral("ok"));
}

TEST(DockArea, TheFillIsTheDefaultBackgroundOfTheWholeArea)
{
    const AfterglowVt::TerminalOptions options;
    auto plain = dock(QVariantList{run(QStringLiteral("x"))});
    EXPECT_EQ(plain->snapshot().defaultBg, AfterglowRender::packRgba(options.defaultBackground));
    // The renderer paints the area's rect in the default background, also
    // past the last whole cell: with the fill as that colour, no strip of
    // the theme's background shows.
    DockArea hex(QStringLiteral("owner@1"), QStringLiteral("bar"), options);
    hex.setContent({{QStringLiteral("edge"), QStringLiteral("top")}, {QStringLiteral("size"), 1}, {QStringLiteral("fill"), QStringLiteral("#102030")},
                    {QStringLiteral("lines"), QVariantList{QVariant(QVariantList{run(QStringLiteral("x"))})}}}, 1);
    hex.setRect(QRect(0, 0, 10 * Cell + 7, Cell), Cell, Cell);
    EXPECT_EQ(hex.snapshot().defaultBg, AfterglowRender::packRgba(0x10, 0x20, 0x30));
    EXPECT_EQ(hex.snapshot().at(5, 0).bg, 0u); // a cell without a background: the default
    DockArea theme(QStringLiteral("owner@1"), QStringLiteral("bar"), options);
    theme.setContent({{QStringLiteral("edge"), QStringLiteral("top")}, {QStringLiteral("size"), 1}, {QStringLiteral("fill"), 4}}, 1);
    EXPECT_EQ(theme.snapshot().defaultBg, AfterglowRender::packRgba(options.basePalette[4]));
    theme.setContent({{QStringLiteral("edge"), QStringLiteral("top")}, {QStringLiteral("size"), 1}, {QStringLiteral("fill"), 196}}, 2);
    EXPECT_EQ(theme.snapshot().defaultBg, AfterglowRender::packRgba(0xff, 0x00, 0x00));
    // A theme change keeps the fill on top.
    AfterglowVt::TerminalOptions dark = options;
    dark.defaultBackground = {1, 2, 3};
    theme.setOptions(dark);
    EXPECT_EQ(theme.snapshot().defaultBg, AfterglowRender::packRgba(0xff, 0x00, 0x00));
    theme.setContent({{QStringLiteral("edge"), QStringLiteral("top")}, {QStringLiteral("size"), 1}}, 3);
    EXPECT_EQ(theme.snapshot().defaultBg, AfterglowRender::packRgba(1, 2, 3));
}

TEST(DockArea, RunsKeepTheirStyleAndColors)
{
    QVariantMap styled = run(QStringLiteral("b"));
    styled.insert(QStringLiteral("bold"), true);
    styled.insert(QStringLiteral("fg"), QStringLiteral("#ff8000"));
    styled.insert(QStringLiteral("bg"), 2);
    auto area = dock(QVariantList{styled, run(QStringLiteral("p"))});
    const auto& snap = area->snapshot();
    EXPECT_TRUE(snap.at(0, 0).attrs & AfterglowVt::Attr::Bold);
    EXPECT_EQ(snap.at(0, 0).fg & 0x00FFFFFFu, AfterglowRender::packRgba(0xff, 0x80, 0x00) & 0x00FFFFFFu);
    EXPECT_EQ(snap.at(0, 0).bg, AfterglowRender::packRgba(AfterglowVt::TerminalOptions{}.basePalette[2]));
    // The next run starts plain again.
    EXPECT_FALSE(snap.at(1, 0).attrs & AfterglowVt::Attr::Bold);
    EXPECT_EQ(snap.at(1, 0).bg, 0u);
}

TEST(DockArea, LinesPastTheRowsDoNotShowAndAResizeLaysTheTextOutAgain)
{
    DockArea area(QStringLiteral("owner@1"), QStringLiteral("bar"), AfterglowVt::TerminalOptions{});
    const QVariantList lines{QVariant(QVariantList{run(QStringLiteral("one")), spacer(), run(QStringLiteral("1"), QStringLiteral("r"))}),
                             QVariant(QVariantList{run(QStringLiteral("two"))}), QVariant(QVariantList{run(QStringLiteral("three"))})};
    area.setContent({{QStringLiteral("edge"), QStringLiteral("bottom")}, {QStringLiteral("size"), 2}, {QStringLiteral("lines"), lines}}, 1);
    area.setRect(QRect(0, 100, 6 * Cell, 2 * Cell), Cell, Cell);
    EXPECT_EQ(rowText(area, 0), QStringLiteral("one  1"));
    EXPECT_EQ(rowText(area, 1), QStringLiteral("two   "));
    EXPECT_EQ(area.text(), QStringLiteral("one  1\ntwo"));
    EXPECT_EQ(area.itemAt(QPoint(5 * Cell + 1, 101)), QStringLiteral("r"));
    // Wider: the spacer takes the new cells and the item moves with its run.
    area.setRect(QRect(0, 100, 9 * Cell, 2 * Cell), Cell, Cell);
    EXPECT_EQ(rowText(area, 0), QStringLiteral("one     1"));
    EXPECT_EQ(area.itemAt(QPoint(5 * Cell + 1, 101)), QString());
    EXPECT_EQ(area.itemAt(QPoint(8 * Cell + 1, 101)), QStringLiteral("r"));
    // Outside the rect, and an area that does not show, have no items.
    EXPECT_EQ(area.itemAt(QPoint(8 * Cell + 1, 99)), QString());
    EXPECT_EQ(area.itemAt(QPoint(9 * Cell, 101)), QString());
    area.setRect(QRect(), Cell, Cell);
    EXPECT_EQ(area.itemAt(QPoint(0, 0)), QString());
}

TEST(DockArea, ASideDockOneColumnWideLaysItsTextOutInThatColumn)
{
    DockArea area(QStringLiteral("owner@1"), QStringLiteral("side"), AfterglowVt::TerminalOptions{});
    const QVariantList lines{QVariant(QVariantList{spacer(), run(QStringLiteral("x"), QStringLiteral("x"))}),
                             QVariant(QVariantList{run(QStringLiteral("日"), QStringLiteral("w"))})};
    area.setContent({{QStringLiteral("edge"), QStringLiteral("left")}, {QStringLiteral("size"), 1}, {QStringLiteral("lines"), lines}}, 1);
    area.setRect(QRect(0, 0, Cell, 4 * Cell), Cell, Cell);
    // The terminal has two columns, and only the first shows.
    EXPECT_EQ(rowText(area, 0).left(1), QStringLiteral("x"));
    EXPECT_EQ(area.itemAt(QPoint(Cell / 2, Cell / 2)), QStringLiteral("x"));
    // A wide character does not fit in one column: its cell is blank.
    EXPECT_EQ(rowText(area, 1), QStringLiteral("  "));
    EXPECT_EQ(area.itemAt(QPoint(Cell / 2, Cell + Cell / 2)), QStringLiteral("w"));
}
