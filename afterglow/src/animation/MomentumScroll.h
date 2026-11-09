// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <deque>

namespace Afterglow {

/// Inertial scrolling after a finger scroll ends, for touchpads and
/// touchscreens: Wayland sends no momentum of its own. The model is
/// kitty's: the speed of the last 150 ms of the gesture, the recent part
/// counting more, then a step every 8 ms that scrolls by the speed and
/// slows it by the friction, until it is below half a pixel.
///
/// Distances are logical pixels in the direction of the scroll events.
/// Times are milliseconds on one steady clock. It holds no timer: the
/// caller advances it while coasting() is true, and stops then.
class MomentumScroll
{
public:
    /// The milliseconds between two steps of the coast.
    static constexpr std::int64_t StepMs = 8;

    /// The fraction of the speed kept at each step, 0 to 1. 0 turns the
    /// coast off; 1 never slows down (the speed is still capped).
    void setFriction(double friction);
    double friction() const noexcept { return m_friction; }

    /// The fingers moved by @p delta at @p ms. A move ends a coast, and
    /// a move against the coast's direction also drops its speed.
    void move(double delta, std::int64_t ms);
    /// The fingers lifted at @p ms. Returns whether the gesture coasts: it
    /// moved more than 10 pixels, and the friction is above 0.
    bool release(std::int64_t ms);
    /// Ends a coast and forgets the gesture: a press, a key, another pane.
    void cancel();

    bool coasting() const noexcept { return m_coasting; }
    /// The distance of the steps that are due by @p ms. When the speed
    /// falls below half a pixel, the coast ends.
    double advance(std::int64_t ms);

private:
    struct Sample
    {
        double delta;
        std::int64_t ms;
    };
    void addSpeed(double speed);

    double m_friction = 0.96;
    std::deque<Sample> m_samples;
    double m_speed = 0; ///< pixels for each step
    double m_displacement = 0; ///< of the gesture, for the 10 pixel rule
    bool m_gesture = false;
    bool m_coasting = false;
    std::int64_t m_gestureStart = 0;
    std::int64_t m_nextStep = 0;
};

} // namespace Afterglow
