// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QRect>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Afterglow {

class Pane;

/// Binary split layout of a window's panes. Leaves hold panes; inner nodes
/// split their rectangle horizontally or vertically at a ratio.
class SplitTree
{
public:
    enum class Direction { Right, Down, Left, Up };
    enum class Neighbour { Left, Right, Up, Down };

    struct Divider
    {
        QRect rect;
        bool vertical = false; ///< separates top from bottom (a horizontal line)
        QRect area; ///< the split's whole rectangle
        const void* node = nullptr; ///< identifies the split for dragDivider()
    };

    explicit SplitTree(Pane* root);

    /// Splits @p target, placing @p added on the @p direction side of it.
    void split(Pane* target, Pane* added, Direction direction);
    /// Removes @p pane; its sibling takes the freed space. Returns false if
    /// the pane was the last one.
    bool remove(Pane* pane);

    /// Lays the tree out in @p area with dividers @p dividerWidth wide.
    void layout(const QRect& area, int dividerWidth);

    bool contains(const Pane* pane) const;
    QRect rectOf(const Pane* pane) const;
    Pane* paneAt(const QPoint& pos) const;
    Pane* neighbour(const Pane* from, Neighbour direction) const;
    void forEachPane(const std::function<void(Pane*, const QRect&)>& fn) const;
    const std::vector<Divider>& dividers() const noexcept { return m_dividers; }
    int paneCount() const;
    void equalize();

    /// The divider within @p tolerance pixels of @p pos, if any.
    std::optional<Divider> dividerAt(const QPoint& pos, int tolerance) const;
    /// Moves a divider so it sits at @p pos, keeping both sides at least
    /// @p minimum pixels. Takes effect on the next layout().
    void dragDivider(const Divider& divider, const QPoint& pos, int dividerWidth, const QSize& minimum);

private:
    struct Node
    {
        Pane* pane = nullptr;
        bool vertical = false; ///< children stacked top/bottom
        double ratio = 0.5;
        std::unique_ptr<Node> first;
        std::unique_ptr<Node> second;
        Node* parent = nullptr;
        QRect rect;
    };

    Node* find(Node* node, const Pane* pane) const;
    void layoutNode(Node* node, const QRect& area, int dividerWidth);
    void visit(const Node* node, const std::function<void(const Node*)>& fn) const;

    std::unique_ptr<Node> m_root;
    std::vector<Divider> m_dividers;
};

} // namespace Afterglow
