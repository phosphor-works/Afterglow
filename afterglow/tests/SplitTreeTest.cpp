// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SplitTree.h"

#include <gtest/gtest.h>

using Afterglow::Pane;
using Afterglow::SplitTree;

namespace {
// The tree never dereferences panes; distinct addresses are enough.
Pane* fake(std::uintptr_t n)
{
    return reinterpret_cast<Pane*>(n * 16);
}
} // namespace

TEST(SplitTree, SplitAndRemove)
{
    SplitTree t(fake(1));
    t.split(fake(1), fake(2), SplitTree::Direction::Right);
    t.layout(QRect(0, 0, 201, 100), 1);
    EXPECT_EQ(t.paneCount(), 2);
    EXPECT_EQ(t.rectOf(fake(1)), QRect(0, 0, 100, 100));
    EXPECT_EQ(t.rectOf(fake(2)), QRect(101, 0, 100, 100));
    ASSERT_EQ(t.dividers().size(), 1u);
    EXPECT_EQ(t.dividers()[0].rect, QRect(100, 0, 1, 100));

    EXPECT_TRUE(t.remove(fake(1)));
    t.layout(QRect(0, 0, 201, 100), 1);
    EXPECT_EQ(t.rectOf(fake(2)), QRect(0, 0, 201, 100));
    EXPECT_FALSE(t.remove(fake(2))); // the last pane stays
}

TEST(SplitTree, DragDividerRespectsMinimum)
{
    SplitTree t(fake(1));
    t.split(fake(1), fake(2), SplitTree::Direction::Right);
    t.layout(QRect(0, 0, 201, 100), 1);
    const auto d = t.dividerAt(QPoint(101, 50), 3);
    ASSERT_TRUE(d.has_value());
    EXPECT_FALSE(d->vertical);

    t.dragDivider(*d, QPoint(150, 50), 1, QSize(40, 20));
    t.layout(QRect(0, 0, 201, 100), 1);
    EXPECT_EQ(t.rectOf(fake(1)).width(), 150);

    t.dragDivider(*d, QPoint(195, 50), 1, QSize(40, 20)); // too far: clamped
    t.layout(QRect(0, 0, 201, 100), 1);
    EXPECT_EQ(t.rectOf(fake(2)).width(), 40);

    EXPECT_FALSE(t.dividerAt(QPoint(20, 50), 3).has_value());
}

TEST(SplitTree, NeighbourAndEqualize)
{
    SplitTree t(fake(1));
    t.split(fake(1), fake(2), SplitTree::Direction::Right);
    t.split(fake(2), fake(3), SplitTree::Direction::Down);
    t.split(fake(1), fake(4), SplitTree::Direction::Right);
    t.layout(QRect(0, 0, 300, 100), 0);
    EXPECT_EQ(t.neighbour(fake(1), SplitTree::Neighbour::Right), fake(4));
    EXPECT_EQ(t.neighbour(fake(3), SplitTree::Neighbour::Up), fake(2));
    EXPECT_EQ(t.neighbour(fake(1), SplitTree::Neighbour::Left), nullptr);

    // Three columns side by side get a third of the width each.
    t.equalize();
    t.layout(QRect(0, 0, 300, 100), 0);
    EXPECT_EQ(t.rectOf(fake(1)).width(), 100);
    EXPECT_EQ(t.rectOf(fake(4)).width(), 100);
    EXPECT_EQ(t.rectOf(fake(2)).width(), 100);
}

TEST(SplitTree, SplitLeftAndUpPutTheNewPaneFirst)
{
    Pane* a = fake(1);
    Pane* b = fake(2);
    Pane* c = fake(3);
    SplitTree t(a);
    t.split(a, b, SplitTree::Direction::Left);
    t.layout(QRect(0, 0, 201, 100), 1);
    EXPECT_LT(t.rectOf(b).x(), t.rectOf(a).x());
    EXPECT_EQ(t.rectOf(a).y(), t.rectOf(b).y());
    t.split(a, c, SplitTree::Direction::Up);
    t.layout(QRect(0, 0, 201, 101), 1);
    EXPECT_LT(t.rectOf(c).y(), t.rectOf(a).y());
    EXPECT_EQ(t.rectOf(c).x(), t.rectOf(a).x());
}

TEST(SplitTree, ZoomPreservesTopologyRatiosAndNavigation)
{
    SplitTree t(fake(1));
    t.split(fake(1), fake(2), SplitTree::Direction::Right);
    t.split(fake(2), fake(3), SplitTree::Direction::Down);
    t.layout(QRect(0, 0, 401, 201), 1);
    ASSERT_TRUE(t.resizePane(fake(1), SplitTree::Neighbour::Right, 40, 1, QSize(40, 20)));
    t.layout(QRect(0, 0, 401, 201), 1);
    const QRect original = t.rectOf(fake(1));
    Pane* neighbour = t.neighbour(fake(1), SplitTree::Neighbour::Right);
    t.setZoomedPane(fake(1));
    EXPECT_EQ(t.paneCount(), 3);
    EXPECT_EQ(t.rectOf(fake(1)), QRect(0, 0, 401, 201));
    EXPECT_TRUE(t.rectOf(fake(2)).isEmpty());
    EXPECT_EQ(t.paneAt(QPoint(350, 150)), fake(1));
    EXPECT_TRUE(t.dividers().empty());
    EXPECT_EQ(t.neighbour(fake(1), SplitTree::Neighbour::Right), neighbour);
    EXPECT_FALSE(t.resizePane(fake(1), SplitTree::Neighbour::Right, 20, 1, QSize(40, 20)));
    t.setZoomedPane(nullptr);
    t.layout(QRect(0, 0, 401, 201), 1);
    EXPECT_EQ(t.rectOf(fake(1)), original);
    EXPECT_EQ(t.dividers().size(), 2u);
    t.setZoomedPane(fake(3));
    t.layout(QRect(0, 0, 801, 401), 1);
    EXPECT_EQ(t.rectOf(fake(3)), QRect(0, 0, 801, 401));
    t.setZoomedPane(nullptr);
    t.layout(QRect(0, 0, 801, 401), 1);
    EXPECT_EQ(t.rectOf(fake(1)).width(), 2 * original.width());
    t.setZoomedPane(fake(3));
    EXPECT_TRUE(t.remove(fake(3)));
    EXPECT_EQ(t.zoomedPane(), nullptr);
}

TEST(SplitTree, KeyboardResizeClampsNestedMinimumSizes)
{
    SplitTree t(fake(1));
    t.split(fake(1), fake(2), SplitTree::Direction::Right);
    t.split(fake(2), fake(3), SplitTree::Direction::Right);
    t.layout(QRect(0, 0, 401, 201), 1);
    EXPECT_TRUE(t.resizePane(fake(1), SplitTree::Neighbour::Right, 1000, 1, QSize(40, 20)));
    t.layout(QRect(0, 0, 401, 201), 1);
    EXPECT_GE(t.rectOf(fake(2)).width(), 40);
    EXPECT_GE(t.rectOf(fake(3)).width(), 40);
    EXPECT_FALSE(t.resizePane(fake(1), SplitTree::Neighbour::Right, 1000, 1, QSize(40, 20)));
    EXPECT_TRUE(t.resizePane(fake(1), SplitTree::Neighbour::Left, 1000, 1, QSize(40, 20)));
    t.layout(QRect(0, 0, 401, 201), 1);
    EXPECT_EQ(t.rectOf(fake(1)).width(), 40);
    t.split(fake(1), fake(4), SplitTree::Direction::Down);
    t.layout(QRect(0, 0, 401, 201), 1);
    EXPECT_TRUE(t.resizePane(fake(4), SplitTree::Neighbour::Up, 30, 1, QSize(40, 20)));
    t.layout(QRect(0, 0, 401, 201), 1);
    EXPECT_EQ(t.rectOf(fake(4)).height(), 130);
    EXPECT_TRUE(t.resizePane(fake(1), SplitTree::Neighbour::Down, 1000, 1, QSize(40, 20)));
    t.layout(QRect(0, 0, 401, 201), 1);
    EXPECT_EQ(t.rectOf(fake(4)).height(), 20);
}

TEST(SplitTree, SwapPreservesTopologyAndClearsZoom)
{
    SplitTree tree(fake(1));
    tree.split(fake(1), fake(2), SplitTree::Direction::Right);
    tree.split(fake(2), fake(3), SplitTree::Direction::Down);
    tree.layout(QRect(0, 0, 301, 201), 1);
    const auto first = tree.rectOf(fake(1)), second = tree.rectOf(fake(3));
    tree.setZoomedPane(fake(1));
    ASSERT_TRUE(tree.swap(fake(1), fake(3)));
    tree.layout(QRect(0, 0, 301, 201), 1);
    EXPECT_EQ(tree.rectOf(fake(3)), first);
    EXPECT_EQ(tree.rectOf(fake(1)), second);
    EXPECT_EQ(tree.zoomedPane(), nullptr);
    EXPECT_EQ(tree.paneCount(), 3);
    EXPECT_FALSE(tree.swap(fake(1), fake(9)));
    EXPECT_FALSE(tree.swap(fake(1), fake(1)));
}
