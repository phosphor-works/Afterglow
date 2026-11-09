// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "NestedLoop.h"

#include <QCoreApplication>
#include <QPointer>
#include <QWindow>
#include <gtest/gtest.h>

#include <optional>

using Afterglow::NestedLoop;

namespace {

void deferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

TEST(NestedLoop, AWindowWithNoLoopGoesAtOnce)
{
    const QPointer<QWindow> window = new QWindow;
    NestedLoop::deleteWhenIdle(window);
    deferredDeletes();
    EXPECT_TRUE(window.isNull());
}

TEST(NestedLoop, AWindowGoesAfterItsLastLoop)
{
    const QPointer<QWindow> window = new QWindow;
    std::optional<NestedLoop> drag(std::in_place, window.data());
    std::optional<NestedLoop> drop(std::in_place, window.data());
    NestedLoop::deleteWhenIdle(window);
    // The deletes that a loop would run while it lasts.
    deferredDeletes();
    EXPECT_FALSE(window.isNull());
    drop.reset();
    deferredDeletes();
    EXPECT_FALSE(window.isNull());
    drag.reset();
    deferredDeletes();
    EXPECT_TRUE(window.isNull());
}

TEST(NestedLoop, ALoopWithoutACloseDeletesNothingAndLeavesNothing)
{
    const QPointer<QWindow> window = new QWindow;
    {
        const NestedLoop loop(window);
    }
    deferredDeletes();
    EXPECT_FALSE(window.isNull());
    // A close after the loop deletes the window at once.
    NestedLoop::deleteWhenIdle(window);
    deferredDeletes();
    EXPECT_TRUE(window.isNull());
}

} // namespace
