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
