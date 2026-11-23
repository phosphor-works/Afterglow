// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorAnimation/AnimatedValue.h>

#include <QPointF>
#include <QRectF>
#include <QString>

namespace Afterglow {

/// A pane's animated presentation: where it is drawn, where its cursor is
/// drawn, its scroll position in fractional rows, and its visual bell.
struct PaneMotion
{
    PhosphorAnimation::AnimatedValue<QRectF> rect;
    PhosphorAnimation::AnimatedValue<QPointF> cursor;
    PhosphorAnimation::AnimatedValue<qreal> scroll;
    PhosphorAnimation::AnimatedValue<qreal> bell; ///< visual bell flash, 1 fading to 0

    bool rectKnown = false;
    QRectF rectTarget;
    bool cursorKnown = false;
    QPointF cursorTarget;
    qreal scrollTarget = 0;
    /// Rows scrolled back, fractional while easing or after a trackpad scroll.
    qreal scrollValue = 0;
    /// Rows that output pushed the view back by while scroll eased: scroll
    /// keeps the rows of its start, and scrollValue is its value plus these.
    /// A restart for each output batch would reset its time and velocity,
    /// and the easing would not end while the output went on.
    qreal scrollRebase = 0;
    /// The viewport offset this pane last gave its terminal, to notice when
    /// the terminal moved it on its own (output pinning, input reset).
    std::size_t appliedOffset = 0;
    /// How far up the last frame drew the rows, in pixels: the fraction of
    /// a row that scrollValue is past appliedOffset. The pointer's cell is
    /// the one drawn under it.
    float scrollShift = 0;
    /// The pointer the program asked for (OSC 22); empty for the default.
    QString pointerShape;
};

} // namespace Afterglow
