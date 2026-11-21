// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "SplitTree.h"

#include <QRect>
#include <QSize>

#include <vector>

namespace Afterglow {

/// A card of the pane overview, for its arrow keys: the pane's rectangle in
/// its own tab's layout, and the index of its tab.
struct OverviewCard
{
    QRect rect;
    int tab = 0;
};

/// The card that an arrow key goes to from the card @p current, in an
/// overview that draws each tab as a picture of its layout (the metadata
/// "overviewNavigation": "direction", as flat_overview.glsl draws it). The
/// tabs are in a grid in tab order: the smallest square grid that holds
/// them, with the last row centred. @p cards are in tab order, as the
/// overview has them, and @p window is the size that each tab is laid out in.
///
/// Within a tab the rule is that of SplitTree::neighbor(), so an arrow key
/// goes where the focus_left action and its like go. When the tab has no pane
/// in that direction, the same rule finds a pane of another tab. -1 when
/// there is none: the highlight then stays.
int overviewNeighbor(const std::vector<OverviewCard>& cards, int current, SplitTree::Neighbor direction,
                     const QSize& window);

} // namespace Afterglow
