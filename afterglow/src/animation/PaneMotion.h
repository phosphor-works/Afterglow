// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorAnimation/AnimatedValue.h>

#include <QPointF>
#include <QRectF>

namespace Afterglow {

/// A pane's animated presentation: where it is drawn, where its cursor is
/// drawn, and its scroll position in fractional rows.
struct PaneMotion
{
    PhosphorAnimation::AnimatedValue<QRectF> rect;
    PhosphorAnimation::AnimatedValue<QPointF> cursor;
    PhosphorAnimation::AnimatedValue<qreal> scroll;

    bool rectKnown = false;
    QRectF rectTarget;
    bool cursorKnown = false;
    QPointF cursorTarget;
    qreal scrollTarget = 0;
    /// Rows scrolled back, fractional while easing or after a trackpad scroll.
    qreal scrollValue = 0;
    /// The viewport offset this pane last gave its terminal, to notice when
    /// the terminal moved it on its own (output pinning, input reset).
    std::size_t appliedOffset = 0;
};

} // namespace Afterglow
