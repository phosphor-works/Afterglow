// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "animation/MomentumScroll.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using Afterglow::MomentumScroll;

namespace {
constexpr double Step = MomentumScroll::StepMs;

/// A gesture that begins at @p start, moves by @p delta five times 8 ms
/// apart, and lifts 8 ms after the last move.
bool flick(MomentumScroll& m, double delta, double start = 0)
{
    m.begin(start);
    for (int i = 0; i < 5; ++i) m.move(delta, start + i * Step);
    return m.release(start + 5 * Step);
}
/// The speed that flick() gives, by kitty's rule: the mean of the five moves
/// and the lift (a sample of 0), weighted 1, 1.2 ... 2 by their time.
double flickSpeed(double delta)
{
    return delta * (1.0 + 1.2 + 1.4 + 1.6 + 1.8) / (1.0 + 1.2 + 1.4 + 1.6 + 1.8 + 2.0);
}
/// kitty's coast as its timer runs it: at each step the speed slows by the
/// friction, and the coast ends once it is below half a pixel. The distance
/// after each step.
std::vector<double> kittySteps(double speed, double friction)
{
    std::vector<double> at;
    double total = 0;
    for (;;) {
        speed *= friction;
        if (std::abs(speed) < 0.5 || at.size() > 100000) break;
        total += speed;
        at.push_back(total);
    }
    return at;
}
/// Advances every @p everyMs from @p from until the coast ends: the distance
/// at each call.
std::vector<double> coast(MomentumScroll& m, double from, double everyMs)
{
    std::vector<double> at;
    double total = 0;
    for (double t = from; m.coasting() && t < from + 60000; t += everyMs) {
        total += m.advance(t);
        at.push_back(total);
    }
    return at;
}
} // namespace

TEST(MomentumScroll, TheCoastIsAtKittysStepsAtEachStepTime)
{
    MomentumScroll m;
    ASSERT_TRUE(flick(m, 20));
    const std::vector<double> kitty = kittySteps(flickSpeed(20), 0.96);
    ASSERT_GT(kitty.size(), 50u);
    // Step n of kitty's coast is n steps after the lift.
    const std::vector<double> ours = coast(m, 5 * Step + Step, Step);
    EXPECT_FALSE(m.coasting());
    ASSERT_EQ(ours.size(), kitty.size());
    for (std::size_t i = 0; i < kitty.size(); ++i) EXPECT_NEAR(ours[i], kitty[i], 1e-9) << "step " << i + 1;
    EXPECT_EQ(m.advance(100000), 0);
}

TEST(MomentumScroll, EachFrameShowsTheCoastAtItsOwnTime)
{
    // A frame every millisecond and a frame every 33: where both look, the
    // coast is at the same place, and it moves between two of kitty's steps.
    MomentumScroll fine, coarse;
    ASSERT_TRUE(flick(fine, -15));
    ASSERT_TRUE(flick(coarse, -15));
    const double lift = 5 * Step;
    const std::vector<double> a = coast(fine, lift + 1, 1);
    const std::vector<double> b = coast(coarse, lift + 33, 33);
    ASSERT_GT(b.size(), 5u);
    for (std::size_t k = 0; k < b.size(); ++k) {
        const std::size_t i = (k + 1) * 33 - 1;
        if (i >= a.size()) break;
        EXPECT_NEAR(a[i], b[k], 1e-9) << "at " << (k + 1) * 33 << " ms";
    }
    EXPECT_NEAR(a.back(), b.back(), 1e-9);
    EXPECT_LT(a.back(), 0);
    // Each millisecond moves, and less than the one before: no frame stands
    // still or jumps between two steps.
    for (std::size_t i = 1; i < a.size(); ++i) {
        const double moved = a[i] - a[i - 1];
        EXPECT_LT(moved, 0) << i;
        if (i > 1) {
            EXPECT_GE(moved, a[i - 1] - a[i - 2] - 1e-12) << i;
        }
    }
}

TEST(MomentumScroll, ALateFrameDelaysTheCoastAndDoesNotJump)
{
    MomentumScroll m;
    ASSERT_TRUE(flick(m, 20));
    const double lift = 5 * Step;
    m.advance(lift + 16);
    // The window was busy for a second: the coast goes on from where it was,
    // by no more than 50 ms of its own time.
    const double late = m.advance(lift + 1016);
    EXPECT_GT(late, 0);
    EXPECT_LT(late, flickSpeed(20) * 50 / Step);
    EXPECT_TRUE(m.coasting());
}

TEST(MomentumScroll, TheLiftCountsAndAFingerThatStoppedCoastsSlower)
{
    MomentumScroll now, held, gone;
    for (MomentumScroll* m : {&now, &held, &gone}) {
        m->begin(0);
        for (int i = 0; i < 5; ++i) m->move(20, i * Step);
    }
    ASSERT_TRUE(now.release(5 * Step));
    // Still for 140 ms: the first moves are older than 150 ms, and the two
    // that count weigh less against the lift.
    ASSERT_TRUE(held.release(4 * Step + 140));
    const double atOnce = now.advance(5 * Step + Step);
    const double afterHold = held.advance(4 * Step + 140 + Step);
    EXPECT_NEAR(atOnce, flickSpeed(20) * 0.96, 1e-9);
    EXPECT_LT(afterHold, atOnce * 0.8);
    EXPECT_GT(afterHold, 0);
    // Still for longer than 150 ms: no move is recent, and only the lift
    // counts.
    EXPECT_FALSE(gone.release(4 * Step + 151));
    EXPECT_FALSE(gone.coasting());
}

TEST(MomentumScroll, ASmallOrShortGestureDoesNotCoast)
{
    MomentumScroll m;
    m.begin(0);
    m.move(4, 0);
    m.move(4, 8);
    EXPECT_FALSE(m.release(16)); // 8 pixels: a touch, not a flick
    m.begin(100);
    m.move(5, 100);
    m.move(5, 108);
    EXPECT_FALSE(m.release(116)); // exactly 10 pixels is not more than 10
    m.begin(200);
    m.move(30, 200);
    m.move(30, 202);
    EXPECT_FALSE(m.release(202)); // 2 ms is not more than 2
    m.begin(300);
    m.move(30, 300);
    m.move(30, 302);
    EXPECT_TRUE(m.release(303));
}

TEST(MomentumScroll, TheRecentMovesCountMore)
{
    MomentumScroll m;
    m.begin(0);
    m.move(10, 0);
    m.move(30, 8);
    ASSERT_TRUE(m.release(16));
    // Weights 1, 1.5 and 2 for the moves at 0 and 8 and the lift at 16.
    const double speed = (10 * 1.0 + 30 * 1.5) / (1.0 + 1.5 + 2.0);
    EXPECT_NEAR(m.advance(16 + Step), speed * 0.96, 1e-9);
}

TEST(MomentumScroll, AReverseInAGestureStartsTheSpeedAgain)
{
    MomentumScroll m;
    m.begin(0);
    m.move(20, 0);
    m.move(20, 8);
    m.move(-30, 16);
    m.move(-30, 24);
    m.move(-30, 32);
    ASSERT_TRUE(m.release(40));
    // Only the moves after the reverse, and the lift: weights 1, 4/3, 5/3, 2.
    const double speed = -30 * (1.0 + 4.0 / 3 + 5.0 / 3) / (1.0 + 4.0 / 3 + 5.0 / 3 + 2.0);
    EXPECT_NEAR(m.advance(40 + Step), speed * 0.96, 1e-9);
}

TEST(MomentumScroll, FrictionZeroTurnsItOffOneKeepsTheSpeedAndNaNChangesNothing)
{
    MomentumScroll off;
    off.setFriction(0);
    EXPECT_FALSE(flick(off, 30));
    MomentumScroll endless;
    endless.setFriction(std::nan(""));
    EXPECT_EQ(endless.friction(), MomentumScroll::DefaultFriction);
    endless.setFriction(7); // clamped to 1
    EXPECT_EQ(endless.friction(), 1.0);
    ASSERT_TRUE(flick(endless, 30));
    // 100 steps, 40 ms apart so that no frame is late.
    double total = 0;
    for (int i = 1; i <= 20; ++i) total += endless.advance(5 * Step + i * 5 * Step);
    EXPECT_NEAR(total, flickSpeed(30) * 100, 1e-6);
    EXPECT_TRUE(endless.coasting());
}

TEST(MomentumScroll, ANewFrictionTakesTheCoastOnFromWhereItIs)
{
    MomentumScroll m;
    m.setFriction(1);
    ASSERT_TRUE(flick(m, 30));
    const double lift = 5 * Step;
    EXPECT_NEAR(m.advance(lift + 5 * Step) + m.advance(lift + 10 * Step), flickSpeed(30) * 10, 1e-9);
    // Set to kitty's default while it coasts: the next step keeps 0.96 of
    // the speed it had, and the coast ends.
    m.setFriction(0.96);
    EXPECT_NEAR(m.advance(lift + 11 * Step), flickSpeed(30) * 0.96, 1e-9);
    coast(m, lift + 12 * Step, Step);
    EXPECT_FALSE(m.coasting());
    // Set to 0 while it coasts: it stops.
    ASSERT_TRUE(flick(m, 30, 1000));
    m.setFriction(0);
    EXPECT_FALSE(m.coasting());
    EXPECT_EQ(m.advance(2000), 0);
}

TEST(MomentumScroll, RepeatedFlicksAddUpToTheCapAndAReverseStartsAgain)
{
    MomentumScroll m;
    m.setFriction(1);
    ASSERT_TRUE(flick(m, 90));
    m.advance(5 * Step + Step);
    // A second flick the same way while it coasts: its speed adds to the
    // coast's, up to 100 for each step.
    ASSERT_TRUE(flick(m, 90, 100));
    EXPECT_NEAR(m.advance(100 + 5 * Step + Step), 100, 1e-9);
    // A finger the other way: its own speed counts alone.
    m.begin(200);
    EXPECT_FALSE(m.coasting());
    for (int i = 0; i < 5; ++i) m.move(-5, 200 + i * Step);
    ASSERT_TRUE(m.release(200 + 5 * Step));
    EXPECT_NEAR(m.advance(200 + 5 * Step + Step), flickSpeed(-5), 1e-9);
}

TEST(MomentumScroll, ALiftWithNoMoveEndsACoast)
{
    MomentumScroll m;
    ASSERT_TRUE(flick(m, 20));
    m.advance(5 * Step + Step);
    EXPECT_FALSE(m.release(5 * Step + 2 * Step));
    EXPECT_FALSE(m.coasting());
    EXPECT_EQ(m.advance(1000), 0);
}

TEST(MomentumScroll, CancelForgetsTheGestureItsMovesAndTheSpeed)
{
    MomentumScroll m;
    ASSERT_TRUE(flick(m, 20));
    m.cancel();
    EXPECT_FALSE(m.coasting());
    EXPECT_EQ(m.advance(1000), 0);
    // The gesture before the cancel does not count towards the next one's
    // 10 pixels.
    m.move(20, 2000);
    m.cancel();
    m.move(3, 2100);
    m.move(3, 2108);
    EXPECT_FALSE(m.release(2116));
    // Nor do its moves towards the next one's speed.
    m.move(20, 2290);
    m.cancel();
    m.move(6, 2300);
    m.move(6, 2308);
    ASSERT_TRUE(m.release(2316));
    EXPECT_NEAR(m.advance(2316 + Step), (6 * 1.0 + 6 * 1.5) / (1.0 + 1.5 + 2.0) * 0.96, 1e-9);
    // Nor does the speed of a coast that it ended.
    ASSERT_TRUE(flick(m, 90, 3000));
    m.cancel();
    ASSERT_TRUE(flick(m, 20, 3100));
    EXPECT_NEAR(m.advance(3100 + 5 * Step + Step), flickSpeed(20) * 0.96, 1e-9);
}
