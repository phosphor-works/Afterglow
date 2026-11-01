// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SplitTree.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace Afterglow {

SplitTree::SplitTree(Pane* root)
    : m_root(std::make_unique<Node>())
{
    m_root->pane = root;
}

SplitTree::Node* SplitTree::find(Node* node, const Pane* pane) const
{
    // Inner nodes hold no pane: null never names one of them.
    if (!node || !pane) {
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

void SplitTree::split(Pane* target, Pane* added, Direction direction, double fraction, int dividerWidth, const QSize& minimum)
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
    // A ratio of exactly 0 or 1 leaves an empty pane and does not save.
    leaf->ratio = std::clamp(before ? fraction : 1.0 - fraction, 0.001, 0.999);
    const int avail = (leaf->vertical ? leaf->rect.height() : leaf->rect.width()) - dividerWidth;
    const int least = leaf->vertical ? minimum.height() : minimum.width();
    if (least > 0 && avail >= 2 * least) {
        leaf->ratio = std::clamp(leaf->ratio, double(least) / avail, 1.0 - double(least) / avail);
    }
    leaf->serial = m_nextSerial++;
    leaf->first = std::move(first);
    leaf->second = std::move(second);
}

bool SplitTree::remove(Pane* pane)
{
    Node* leaf = find(m_root.get(), pane);
    if (!leaf || leaf == m_root.get()) {
        return false;
    }
    if (pane == m_zoomed) {
        m_zoomed = nullptr;
    }
    Node* parent = leaf->parent;
    std::unique_ptr<Node> sibling = std::move(parent->first.get() == leaf ? parent->second : parent->first);
    // The sibling replaces the parent in place.
    parent->pane = sibling->pane;
    parent->vertical = sibling->vertical;
    parent->ratio = sibling->ratio;
    // The node now holds the sibling's split, not the one it held: a divider
    // drag that started on the old split must not move this one.
    parent->serial = sibling->serial;
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

bool SplitTree::swap(Pane* first, Pane* second)
{
    auto* a = find(m_root.get(), first);
    auto* b = find(m_root.get(), second);
    if (!a || !b || !a->pane || !b->pane || a == b) return false;
    std::swap(a->pane, b->pane);
    m_zoomed = nullptr;
    m_dividers.clear();
    return true;
}

bool SplitTree::rotate(bool clockwise)
{
    std::vector<Node*> leaves;
    const std::function<void(Node*)> collect = [&](Node* node) {
        if (node->pane) leaves.push_back(node);
        else { collect(node->first.get()); collect(node->second.get()); }
    };
    collect(m_root.get());
    if (leaves.size() < 2) return false;
    std::vector<Pane*> panes;
    for (const Node* leaf : leaves) panes.push_back(leaf->pane);
    if (clockwise) std::rotate(panes.rbegin(), panes.rbegin() + 1, panes.rend());
    else std::rotate(panes.begin(), panes.begin() + 1, panes.end());
    for (std::size_t i = 0; i < leaves.size(); ++i) leaves[i]->pane = panes[i];
    m_zoomed = nullptr;
    m_dividers.clear();
    return true;
}

void SplitTree::layout(const QRect& area, int dividerWidth)
{
    m_dividers.clear();
    layoutNode(m_root.get(), area, dividerWidth);
    if (m_zoomed) {
        m_dividers.clear();
    }
}

void SplitTree::setZoomedPane(Pane* pane)
{
    if (!pane || contains(pane)) {
        m_zoomed = pane;
        m_dividers.clear();
    }
}

QSize SplitTree::minimumSize(const Node* node, int dividerWidth, const QSize& minimum) const
{
    if (node->pane) return minimum;
    const QSize a = minimumSize(node->first.get(), dividerWidth, minimum);
    const QSize b = minimumSize(node->second.get(), dividerWidth, minimum);
    // Preserve descendant ratios while ensuring their smallest leaves fit.
    const double ratio = std::clamp(node->ratio, 0.001, 0.999);
    const auto extent = [ratio, dividerWidth](int first, int second) {
        const double size = std::ceil(std::max(first / ratio, second / (1 - ratio))) + dividerWidth;
        return int(std::min(size, double(std::numeric_limits<int>::max() / 2)));
    };
    if (node->vertical) {
        return {std::max(a.width(), b.width()), extent(a.height(), b.height())};
    }
    return {extent(a.width(), b.width()), std::max(a.height(), b.height())};
}

bool SplitTree::resizePane(Pane* pane, Neighbour direction, int pixels, int dividerWidth, const QSize& minimum)
{
    if (m_zoomed || pixels <= 0) return false;
    Node* leaf = find(m_root.get(), pane);
    if (!leaf) return false;
    const bool vertical = direction == Neighbour::Up || direction == Neighbour::Down;
    const bool positive = direction == Neighbour::Right || direction == Neighbour::Down;
    Node* target = nullptr;
    for (Node* child = leaf; child->parent; child = child->parent) {
        Node* parent = child->parent;
        if (parent->vertical != vertical) continue;
        if (!target) target = parent;
        if ((parent->first.get() == child) == positive) {
            target = parent;
            break;
        }
    }
    if (!target) return false;
    const double before = target->ratio;
    const int available = (vertical ? target->rect.height() : target->rect.width()) - dividerWidth;
    const int offset = int(std::lround(available * target->ratio)) + (positive ? pixels : -pixels);
    const QPoint pos = target->rect.topLeft() + (vertical ? QPoint(0, offset) : QPoint(offset, 0));
    dragDivider({{}, vertical, target->rect, target, target->serial}, pos, dividerWidth, minimum);
    return before != target->ratio;
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
        m_dividers.push_back({d, true, area, node, node->serial});
        layoutNode(node->first.get(), a, dividerWidth);
        layoutNode(node->second.get(), b, dividerWidth);
    } else {
        const int avail = std::max(0, area.width() - dividerWidth);
        const int w1 = int(std::lround(avail * node->ratio));
        const QRect a(area.x(), area.y(), w1, area.height());
        const QRect d(area.x() + w1, area.y(), dividerWidth, area.height());
        const QRect b(d.right() + 1, area.y(), avail - w1, area.height());
        m_dividers.push_back({d, false, area, node, node->serial});
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
    if (m_zoomed) return;
    // The split may be gone if the tree changed during the drag, or another
    // split may have taken its node's place when a pane closed.
    std::function<Node*(Node*)> find = [&](Node* n) -> Node* {
        if (!n || n->pane) {
            return nullptr;
        }
        if (n == divider.node) {
            return n->serial == divider.serial ? n : nullptr;
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
    const QSize a = minimumSize(node->first.get(), dividerWidth, minimum);
    const QSize b = minimumSize(node->second.get(), dividerWidth, minimum);
    const int minFirst = node->vertical ? a.height() : a.width();
    const int minSecond = node->vertical ? b.height() : b.width();
    if (avail < minFirst + minSecond) {
        return;
    }
    const int offset = node->vertical ? pos.y() - area.top() : pos.x() - area.left();
    if (avail > 0) node->ratio = double(std::clamp(offset, minFirst, avail - minSecond)) / double(avail);
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
    if (m_zoomed) return pane == m_zoomed ? m_root->rect : QRect();
    const Node* n = find(m_root.get(), pane);
    return n ? n->rect : QRect();
}

Pane* SplitTree::paneAt(const QPoint& pos) const
{
    if (m_zoomed) return m_root->rect.contains(pos) ? m_zoomed : nullptr;
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
    const Node* source = find(m_root.get(), from);
    const QRect src = source ? source->rect : QRect();
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
    visit(m_root.get(), [&](const Node* n) { fn(n->pane, m_zoomed ? rectOf(n->pane) : n->rect); });
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

QJsonObject SplitTree::snapshot(const std::function<QJsonObject(Pane*)>& pane) const
{
    std::function<QJsonObject(const Node*)> encode = [&](const Node* node) -> QJsonObject {
        if (node->pane) return {{QStringLiteral("pane"), pane(node->pane)}};
        return {{QStringLiteral("direction"), node->vertical ? QStringLiteral("down") : QStringLiteral("right")},
                {QStringLiteral("ratio"), node->ratio}, {QStringLiteral("first"), encode(node->first.get())},
                {QStringLiteral("second"), encode(node->second.get())}};
    };
    return encode(m_root.get());
}

std::unique_ptr<SplitTree> SplitTree::restore(const QJsonObject& tree, const std::function<Pane*(const QJsonObject&)>& factory)
{
    auto result = std::make_unique<SplitTree>(nullptr);
    std::function<std::unique_ptr<Node>(const QJsonObject&, Node*)> decode = [&](const QJsonObject& value, Node* parent) {
        auto node = std::make_unique<Node>();
        node->parent = parent;
        if (value.contains(QStringLiteral("pane"))) node->pane = factory(value.value(QStringLiteral("pane")).toObject());
        else {
            node->vertical = value.value(QStringLiteral("direction")).toString() == QLatin1String("down");
            node->ratio = value.value(QStringLiteral("ratio")).toDouble();
            node->serial = result->m_nextSerial++;
            node->first = decode(value.value(QStringLiteral("first")).toObject(), node.get());
            node->second = decode(value.value(QStringLiteral("second")).toObject(), node.get());
        }
        return node;
    };
    result->m_root = decode(tree, nullptr);
    return result;
}

} // namespace Afterglow
