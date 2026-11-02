// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DockArea.h"

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

QString rowText(const std::unique_ptr<DockArea>& area)
{
    const auto& snap = area->snapshot();
    QString text;
    for (int x = 0; x < snap.cols; ++x) {
        const char32_t cp = snap.cells[std::size_t(x)].cp;
        text += cp ? QString::fromUcs4(&cp, 1) : QStringLiteral(" ");
    }
    return text;
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
    EXPECT_TRUE(area.setContent(top, 1));
    QVariantMap text = top;
    text.insert(QStringLiteral("lines"), QVariantList{QVariantList{run(QStringLiteral("new"))}});
    EXPECT_FALSE(area.setContent(text, 2));
    EXPECT_EQ(area.order(), 1u);
    QVariantMap bottom = top;
    bottom.insert(QStringLiteral("edge"), QStringLiteral("bottom"));
    EXPECT_TRUE(area.setContent(bottom, 3));
    EXPECT_EQ(area.order(), 3u);
}
