// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OverviewNavigation.h"

#include <QRectF>

#include <cmath>
#include <limits>

namespace Afterglow {

int overviewNeighbor(const std::vector<OverviewCard>& cards, int current, SplitTree::Neighbor direction,
                     const QSize& window)
{
    if (current < 0 || current >= int(cards.size()) || window.isEmpty())
        return -1;
    // The grid of tabs, as flat_overview.glsl lays it out. The cards are in
    // tab order, so the first and the last give the tabs that have cards.
    const int first = cards.front().tab;
    const int tabs = cards.back().tab - first + 1;
    int columns = 1;
    while (columns * columns < tabs)
        ++columns;
    const int rows = (tabs + columns - 1) / columns;
    // Where a card is among the tabs: its rectangle in its tab, moved to its
    // tab's cell. A cell has the size of the window, so the distances
    // between panes of two tabs compare with those inside one tab.
    auto placed = [&](const OverviewCard& card) {
        const int rank = card.tab - first;
        const int row = rank / columns;
        const int inRow = row == rows - 1 ? tabs - row * columns : columns;
        const double column = rank % columns + (columns - inRow) * 0.5;
        return QRectF(card.rect).translated(column * window.width(), double(row) * window.height());
    };
    const QRectF from = placed(cards[std::size_t(current)]);
    // SplitTree::neighbor()'s rule: a pane that lies wholly past this one's
    // edge, the nearest first, and among equals the one most in line.
    auto score = [&](const QRectF& to) {
        double along = 0, across = 0;
        switch (direction) {
        case SplitTree::Neighbor::Left:
            along = from.left() - to.right();
            across = std::abs(to.center().y() - from.center().y());
            break;
        case SplitTree::Neighbor::Right:
            along = to.left() - from.right();
            across = std::abs(to.center().y() - from.center().y());
            break;
        case SplitTree::Neighbor::Up:
            along = from.top() - to.bottom();
            across = std::abs(to.center().x() - from.center().x());
            break;
        case SplitTree::Neighbor::Down:
            along = to.top() - from.bottom();
            across = std::abs(to.center().x() - from.center().x());
            break;
        }
        // A pane that touches the edge is past it, as for the whole pixels of
        // SplitTree: panes with no divider between them, and the panes at
        // the facing edges of two tabs.
        return along >= 0 ? (along + 1) * 4 + across : -1.0;
    };
    // A pane of this tab first: a far pane of the tab must not lose to a
    // near pane of the next tab.
    for (const bool sameTab : {true, false}) {
        int best = -1;
        double bestScore = std::numeric_limits<double>::max();
        for (int i = 0; i < int(cards.size()); ++i) {
            if (i == current || (cards[std::size_t(i)].tab == cards[std::size_t(current)].tab) != sameTab)
                continue;
            const double s = score(placed(cards[std::size_t(i)]));
            if (s >= 0 && s < bestScore) {
                bestScore = s;
                best = i;
            }
        }
        if (best >= 0)
            return best;
    }
    return -1;
}

} // namespace Afterglow
