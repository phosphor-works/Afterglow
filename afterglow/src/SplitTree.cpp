// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SplitTree.h"

#include <cmath>
#include <limits>

namespace Afterglow {

SplitTree::SplitTree(Pane* root)
    : m_root(std::make_unique<Node>())
{
    m_root->pane = root;
}

SplitTree::Node* SplitTree::find(Node* node, const Pane* pane) const
{
    if (!node) {
        return nullptr;
    }
    if (node->pane == pane) {
        return node;
    }
    if (Node* n = find(node->first.get(), pane)) {
        return n;
    }
    return find(node->second.get(), pane);
}

void SplitTree::split(Pane* target, Pane* added, Direction direction)
{
    Node* leaf = find(m_root.get(), target);
    if (!leaf || !leaf->pane) {
        return;
    }
    // Left and up put the new pane first.
    const bool before = direction == Direction::Left || direction == Direction::Up;
    auto first = std::make_unique<Node>();
    first->pane = before ? added : leaf->pane;
    first->parent = leaf;
    auto second = std::make_unique<Node>();
    second->pane = before ? leaf->pane : added;
    second->parent = leaf;
    leaf->pane = nullptr;
    leaf->vertical = direction == Direction::Down || direction == Direction::Up;
    leaf->ratio = 0.5;
    leaf->first = std::move(first);
    leaf->second = std::move(second);
}

bool SplitTree::remove(Pane* pane)
{
    Node* leaf = find(m_root.get(), pane);
    if (!leaf || leaf == m_root.get()) {
        return false;
    }
    Node* parent = leaf->parent;
    std::unique_ptr<Node> sibling = std::move(parent->first.get() == leaf ? parent->second : parent->first);
    // The sibling replaces the parent in place.
    parent->pane = sibling->pane;
    parent->vertical = sibling->vertical;
    parent->ratio = sibling->ratio;
    parent->first = std::move(sibling->first);
    parent->second = std::move(sibling->second);
    if (parent->first) {
        parent->first->parent = parent;
    }
    if (parent->second) {
        parent->second->parent = parent;
    }
    return true;
}

void SplitTree::layout(const QRect& area, int dividerWidth)
{
    m_dividers.clear();
    layoutNode(m_root.get(), area, dividerWidth);
}

void SplitTree::layoutNode(Node* node, const QRect& area, int dividerWidth)
{
    node->rect = area;
    if (node->pane) {
        return;
    }
    if (node->vertical) {
        const int avail = std::max(0, area.height() - dividerWidth);
        const int h1 = int(std::lround(avail * node->ratio));
        const QRect a(area.x(), area.y(), area.width(), h1);
        const QRect d(area.x(), area.y() + h1, area.width(), dividerWidth);
        const QRect b(area.x(), d.bottom() + 1, area.width(), avail - h1);
        m_dividers.push_back({d, true, area, node});
        layoutNode(node->first.get(), a, dividerWidth);
        layoutNode(node->second.get(), b, dividerWidth);
    } else {
        const int avail = std::max(0, area.width() - dividerWidth);
        const int w1 = int(std::lround(avail * node->ratio));
        const QRect a(area.x(), area.y(), w1, area.height());
        const QRect d(area.x() + w1, area.y(), dividerWidth, area.height());
        const QRect b(d.right() + 1, area.y(), avail - w1, area.height());
        m_dividers.push_back({d, false, area, node});
        layoutNode(node->first.get(), a, dividerWidth);
        layoutNode(node->second.get(), b, dividerWidth);
    }
}

std::optional<SplitTree::Divider> SplitTree::dividerAt(const QPoint& pos, int tolerance) const
{
    for (const Divider& d : m_dividers) {
        if (d.rect.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(pos)) {
            return d;
        }
    }
    return std::nullopt;
}

void SplitTree::dragDivider(const Divider& divider, const QPoint& pos, int dividerWidth, const QSize& minimum)
{
    // The node may be gone if the tree changed during the drag.
    std::function<Node*(Node*)> find = [&](Node* n) -> Node* {
        if (!n || n->pane) {
            return nullptr;
        }
        if (n == divider.node) {
            return n;
        }
        if (Node* f = find(n->first.get())) {
            return f;
        }
        return find(n->second.get());
    };
    Node* node = find(m_root.get());
    if (!node) {
        return;
    }
    const QRect& area = node->rect;
    const int avail = (node->vertical ? area.height() : area.width()) - dividerWidth;
    const int minSide = node->vertical ? minimum.height() : minimum.width();
    if (avail <= 2 * minSide) {
        return;
    }
    const int offset = node->vertical ? pos.y() - area.top() : pos.x() - area.left();
    node->ratio = double(std::clamp(offset, minSide, avail - minSide)) / double(avail);
}

void SplitTree::visit(const Node* node, const std::function<void(const Node*)>& fn) const
{
    if (!node) {
        return;
    }
    if (node->pane) {
        fn(node);
        return;
    }
    visit(node->first.get(), fn);
    visit(node->second.get(), fn);
}

bool SplitTree::contains(const Pane* pane) const
{
    return find(m_root.get(), pane) != nullptr;
}

QRect SplitTree::rectOf(const Pane* pane) const
{
    const Node* n = find(m_root.get(), pane);
    return n ? n->rect : QRect();
}

Pane* SplitTree::paneAt(const QPoint& pos) const
{
    Pane* hit = nullptr;
    visit(m_root.get(), [&](const Node* n) {
        if (n->rect.contains(pos)) {
            hit = n->pane;
        }
    });
    return hit;
}

Pane* SplitTree::neighbour(const Pane* from, Neighbour direction) const
{
    const QRect src = rectOf(from);
    if (src.isNull()) {
        return nullptr;
    }
    const QPoint centre = src.center();
    Pane* best = nullptr;
    long bestScore = std::numeric_limits<long>::max();
    visit(m_root.get(), [&](const Node* n) {
        if (n->pane == from) {
            return;
        }
        const QRect r = n->rect;
        bool candidate = false;
        long along = 0;
        long across = 0;
        switch (direction) {
        case Neighbour::Left:
            candidate = r.right() < src.left();
            along = src.left() - r.right();
            across = std::abs(r.center().y() - centre.y());
            break;
        case Neighbour::Right:
            candidate = r.left() > src.right();
            along = r.left() - src.right();
            across = std::abs(r.center().y() - centre.y());
            break;
        case Neighbour::Up:
            candidate = r.bottom() < src.top();
            along = src.top() - r.bottom();
            across = std::abs(r.center().x() - centre.x());
            break;
        case Neighbour::Down:
            candidate = r.top() > src.bottom();
            along = r.top() - src.bottom();
            across = std::abs(r.center().x() - centre.x());
            break;
        }
        if (candidate) {
            const long score = along * 4 + across;
            if (score < bestScore) {
                bestScore = score;
                best = n->pane;
            }
        }
    });
    return best;
}

void SplitTree::forEachPane(const std::function<void(Pane*, const QRect&)>& fn) const
{
    visit(m_root.get(), [&](const Node* n) { fn(n->pane, n->rect); });
}

int SplitTree::paneCount() const
{
    int count = 0;
    visit(m_root.get(), [&](const Node*) { ++count; });
    return count;
}

void SplitTree::equalize()
{
    // Weight of a subtree along one axis: panes stacked along that axis count
    // individually, a split across it counts as one slot.
    std::function<int(const Node*, bool)> weight = [&](const Node* n, bool vertical) -> int {
        if (n->pane || n->vertical != vertical) {
            return 1;
        }
        return weight(n->first.get(), vertical) + weight(n->second.get(), vertical);
    };
    std::function<void(Node*)> walk = [&](Node* n) {
        if (n->pane) {
            return;
        }
        const int a = weight(n->first.get(), n->vertical);
        const int b = weight(n->second.get(), n->vertical);
        n->ratio = double(a) / double(a + b);
        walk(n->first.get());
        walk(n->second.get());
    };
    walk(m_root.get());
}

} // namespace Afterglow
