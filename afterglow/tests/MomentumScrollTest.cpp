// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "animation/MomentumScroll.h"

#include <gtest/gtest.h>

using Afterglow::MomentumScroll;

namespace {
/// Five moves of @p delta, 8 ms apart from @p start, then the release.
bool flick(MomentumScroll& m, double delta, std::int64_t start = 0)
{
    for (int i = 0; i < 5; ++i) m.move(delta, start + i * 8);
    return m.release(start + 40);
}
/// Advances until the coast ends, at most @p limitMs: the whole distance.
double coast(MomentumScroll& m, std::int64_t from, std::int64_t stepMs = MomentumScroll::StepMs, std::int64_t limitMs = 60000)
{
    double total = 0;
    for (std::int64_t t = from; m.coasting() && t <= from + limitMs; t += stepMs) total += m.advance(t);
    return total;
}
} // namespace

TEST(MomentumScroll, AFlickCoastsSlowerEachStepAndStops)
{
    MomentumScroll m;
    ASSERT_TRUE(flick(m, 20));
    // The first step: the speed of the flick (20 a step), less the friction.
    EXPECT_NEAR(m.advance(40), 20 * 0.96, 1e-9);
    double previous = 20 * 0.96;
    for (std::int64_t t = 48; t < 400; t += 8) {
        const double step = m.advance(t);
        if (!m.coasting()) break;
        EXPECT_LT(step, previous);
        EXPECT_GT(step, 0);
        previous = step;
    }
    const double rest = coast(m, 400);
    EXPECT_FALSE(m.coasting());
    // 20 a step with friction 0.96 coasts about 480 pixels in all, and stops
    // below half a pixel in about 90 steps (0.7 s).
    EXPECT_GT(rest, 0);
    EXPECT_LT(rest, 100);
    EXPECT_EQ(m.advance(100000), 0);
}

TEST(MomentumScroll, TheDistanceDoesNotDependOnTheFrameRate)
{
    MomentumScroll fine, coarse;
    ASSERT_TRUE(flick(fine, -15));
    ASSERT_TRUE(flick(coarse, -15));
    const double a = coast(fine, 40, 1);
    const double b = coast(coarse, 40, 33);
    EXPECT_LT(a, 0);
    EXPECT_NEAR(a, b, 1e-9);
}

TEST(MomentumScroll, ASmallOrSlowGestureDoesNotCoast)
{
    MomentumScroll m;
    m.move(4, 0);
    m.move(4, 8);
    EXPECT_FALSE(m.release(16)); // 8 pixels: a touch, not a flick
    // A finger that stopped before it lifted: no sample is recent.
    m.move(30, 0);
    m.move(30, 8);
    EXPECT_FALSE(m.release(400));
    EXPECT_FALSE(m.coasting());
}

TEST(MomentumScroll, FrictionZeroTurnsItOffAndOneKeepsTheSpeed)
{
    MomentumScroll off;
    off.setFriction(0);
    EXPECT_FALSE(flick(off, 30));
    MomentumScroll endless;
    endless.setFriction(7); // clamped to 1
    EXPECT_EQ(endless.friction(), 1.0);
    ASSERT_TRUE(flick(endless, 30));
    EXPECT_NEAR(endless.advance(40 + 8 * 100), 30.0 * 101, 1e-6);
    EXPECT_TRUE(endless.coasting());
}

TEST(MomentumScroll, RepeatedFlicksAddUpToTheCapAndAReverseStopsTheCoast)
{
    MomentumScroll m;
    m.setFriction(1);
    ASSERT_TRUE(flick(m, 60));
    m.advance(40);
    ASSERT_TRUE(flick(m, 60, 100)); // a second flick the same way, while it coasts
    EXPECT_NEAR(m.advance(140), 100, 1e-9); // the speed is capped at 100 a step
    // A finger the other way stops the coast, and its own speed counts alone.
    m.move(-5, 200);
    EXPECT_FALSE(m.coasting());
    for (int i = 1; i < 5; ++i) m.move(-5, 200 + i * 8);
    ASSERT_TRUE(m.release(240));
    EXPECT_NEAR(m.advance(240), -5, 1e-9);
}

TEST(MomentumScroll, CancelEndsTheCoastAndTheGesture)
{
    MomentumScroll m;
    ASSERT_TRUE(flick(m, 20));
    m.cancel();
    EXPECT_FALSE(m.coasting());
    EXPECT_EQ(m.advance(1000), 0);
    m.move(20, 2000);
    m.cancel();
    EXPECT_FALSE(m.release(2010)); // the gesture before the cancel is gone
}
