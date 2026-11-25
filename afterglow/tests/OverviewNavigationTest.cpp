// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// The arrow keys of an overview that draws each tab as a picture of its
// layout: the pane in the key's direction, in the tab and then among the
// tabs.

#include "OverviewNavigation.h"

#include <gtest/gtest.h>

using Afterglow::OverviewCard;
using Afterglow::overviewNeighbor;
using Afterglow::Pane;
using Afterglow::SplitTree;
using Neighbor = Afterglow::SplitTree::Neighbor;

namespace {
// The tree never dereferences panes; distinct addresses are enough.
Pane* fake(std::uintptr_t n)
{
    return reinterpret_cast<Pane*>(n * 16);
}
const QSize Window(400, 300);
const QRect Whole(QPoint(), Window);
constexpr Neighbor Directions[]{Neighbor::Left, Neighbor::Right, Neighbor::Up, Neighbor::Down};
} // namespace

TEST(OverviewNavigation, InsideATabAnArrowGoesWhereTheFocusActionsGo)
{
    // 1 | 2      with 4 split again to the right: five panes, some of them
    // 3 | 4 5    next to two others.
    SplitTree tree(fake(1));
    tree.split(fake(1), fake(2), SplitTree::Direction::Right);
    tree.split(fake(1), fake(3), SplitTree::Direction::Down);
    tree.split(fake(2), fake(4), SplitTree::Direction::Down);
    tree.split(fake(4), fake(5), SplitTree::Direction::Right);
    for (const int divider : {0, 2}) {
        SCOPED_TRACE(divider);
        tree.layout(Whole, divider);
        std::vector<Pane*> panes;
        std::vector<OverviewCard> cards;
        tree.forEachPane([&](Pane* pane, const QRect& rect) {
            panes.push_back(pane);
            cards.push_back({rect, 0});
        });
        ASSERT_EQ(cards.size(), 5u);
        for (int i = 0; i < int(cards.size()); ++i) {
            for (const Neighbor direction : Directions) {
                const Pane* expected = tree.neighbor(panes[std::size_t(i)], direction);
                const int found = overviewNeighbor(cards, i, direction, Window);
                EXPECT_EQ(found < 0 ? nullptr : panes[std::size_t(found)], expected)
                    << "from pane " << i << ", direction " << int(direction);
            }
        }
    }
}

TEST(OverviewNavigation, AnArrowGoesOnToTheNextTabInTheGridOfTabs)
{
    // Three tabs of one pane: two in the first row, and the third centred
    // below them. Their indexes need not start at 0.
    const std::vector<OverviewCard> cards{{Whole, 3}, {Whole, 4}, {Whole, 5}};
    EXPECT_EQ(overviewNeighbor(cards, 0, Neighbor::Right, Window), 1);
    EXPECT_EQ(overviewNeighbor(cards, 1, Neighbor::Left, Window), 0);
    EXPECT_EQ(overviewNeighbor(cards, 0, Neighbor::Down, Window), 2);
    EXPECT_EQ(overviewNeighbor(cards, 1, Neighbor::Down, Window), 2);
    // Both tabs of the first row are as far from the centred one: the first wins.
    EXPECT_EQ(overviewNeighbor(cards, 2, Neighbor::Up, Window), 0);
    // No tab there: the highlight stays.
    EXPECT_EQ(overviewNeighbor(cards, 0, Neighbor::Left, Window), -1);
    EXPECT_EQ(overviewNeighbor(cards, 0, Neighbor::Up, Window), -1);
    EXPECT_EQ(overviewNeighbor(cards, 2, Neighbor::Down, Window), -1);
    // The centred tab overlaps both columns: nothing lies wholly beside it.
    EXPECT_EQ(overviewNeighbor(cards, 2, Neighbor::Left, Window), -1);
    EXPECT_EQ(overviewNeighbor(cards, 2, Neighbor::Right, Window), -1);
}

TEST(OverviewNavigation, AnArrowEntersATabAtThePaneThatFacesIt)
{
    // Two tabs side by side, each split top and bottom.
    const QRect top(0, 0, 400, 149), bottom(0, 151, 400, 149);
    const std::vector<OverviewCard> cards{{top, 0}, {bottom, 0}, {top, 1}, {bottom, 1}};
    EXPECT_EQ(overviewNeighbor(cards, 0, Neighbor::Right, Window), 2);
    EXPECT_EQ(overviewNeighbor(cards, 1, Neighbor::Right, Window), 3);
    EXPECT_EQ(overviewNeighbor(cards, 3, Neighbor::Left, Window), 1);
    EXPECT_EQ(overviewNeighbor(cards, 0, Neighbor::Down, Window), 1);
    EXPECT_EQ(overviewNeighbor(cards, 3, Neighbor::Up, Window), 2);
}

TEST(OverviewNavigation, APaneOfTheTabComesBeforeANearerPaneOfTheNextTab)
{
    // The pane at the far corner of this tab is further in line than the
    // pane of the next tab, which starts where this tab ends.
    const QRect here(0, 0, 100, 100), corner(350, 250, 50, 50);
    const std::vector<OverviewCard> cards{{here, 0}, {corner, 0}, {QRect(0, 0, 400, 100), 1}};
    EXPECT_EQ(overviewNeighbor(cards, 0, Neighbor::Right, Window), 1);
    // From the corner, the next tab is the only pane to the right.
    EXPECT_EQ(overviewNeighbor(cards, 1, Neighbor::Right, Window), 2);
}

TEST(OverviewNavigation, NothingToGoToGivesNoCard)
{
    EXPECT_EQ(overviewNeighbor({}, 0, Neighbor::Right, Window), -1);
    const std::vector<OverviewCard> one{{Whole, 0}};
    for (const Neighbor direction : Directions)
        EXPECT_EQ(overviewNeighbor(one, 0, direction, Window), -1);
    EXPECT_EQ(overviewNeighbor(one, 1, Neighbor::Right, Window), -1);
    EXPECT_EQ(overviewNeighbor(one, -1, Neighbor::Right, Window), -1);
    EXPECT_EQ(overviewNeighbor(one, 0, Neighbor::Right, QSize()), -1);
}
