// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>

namespace Afterglow {

/// Inertial scrolling after a finger scroll ends, for touchpads and
/// touchscreens: Wayland sends no momentum of its own. The model is
/// kitty's (glfw/momentum-scroll.c): the speed is a weighted mean of the
/// moves in the last 150 ms of the gesture and of the lift, the recent ones
/// counting more. Then every 8 ms the speed slows by the friction and the
/// view scrolls by it, until the speed is below half a pixel.
///
/// The speed is in pixels for each move, as kitty reads it, not for each
/// millisecond. The position between two of kitty's steps is a fraction of
/// a step, so that each frame shows where the coast is at its own time.
///
/// Distances are logical pixels in the direction of the scroll events, on
/// one axis: nothing in Afterglow scrolls sideways. Times are milliseconds
/// on one steady clock. It holds no timer: the caller advances it at each
/// frame while coasting() is true.
class MomentumScroll
{
public:
    /// The milliseconds between two of kitty's steps.
    static constexpr double StepMs = 8;
    /// The part of the speed that each step keeps, unless set: kitty's
    /// momentum_scroll, and Mouse.momentumScroll's default.
    static constexpr double DefaultFriction = 0.96;

    /// The part of the speed kept at each step, 0 to 1. 0 turns the coast
    /// off, and 1 never slows down (the speed is still capped). A value
    /// that is not a number changes nothing. A coast under way goes on from
    /// where it is, with the new value.
    void setFriction(double friction);
    double friction() const noexcept { return m_friction; }

    /// Fingers came down for a new gesture at @p ms. A coast stops, but
    /// keeps its speed for the next flick, so repeated flicks go faster.
    void begin(double ms);
    /// The fingers moved by @p delta at @p ms. A coast stops, as at
    /// begin(). A move against the direction of the moves before it starts
    /// the speed again.
    void move(double delta, double ms);
    /// The fingers lifted at @p ms. Returns whether the gesture coasts: it
    /// moved more than 10 pixels in more than 2 ms, with a move in its last
    /// 150 ms, the friction is above 0, and the first step of the coast is
    /// half a pixel or more. When it does not coast, any speed is gone.
    bool release(double ms);
    /// Ends a coast and forgets the gesture and its speed.
    void cancel();

    /// Fingers are down: begin() or a move came, and no release since.
    bool gesture() const noexcept { return m_gesture; }
    bool coasting() const noexcept { return m_coasting; }
    /// The distance that the coast moved from the last call to @p ms. The
    /// coast ends when its speed falls below half a pixel for each step.
    double advance(double ms);

private:
    struct Sample
    {
        double delta;
        double ms;
    };
    void addSpeed(double speed);
    /// The speed of the coast at its present position.
    double coastSpeed() const;
    /// Starts the coast's curve again from its present position and speed.
    void startCoast(double speed);
    void trimSamples(double ms);

    double m_friction = DefaultFriction;
    std::deque<Sample> m_samples;
    double m_speed = 0; ///< pixels for each step, kept between a coast and the next flick
    double m_displacement = 0; ///< of the gesture, for the 10 pixel rule: net, as kitty's
    bool m_gesture = false;
    bool m_coasting = false;
    double m_gestureStart = 0;
    // The coast: its speed at the start, the steps before it ends, the
    // time on its own clock and the distance it has given.
    double m_coastStart = 0;
    double m_coastSteps = 0;
    double m_coastMs = 0;
    double m_coastLastMs = 0;
    double m_travelled = 0;
};

} // namespace Afterglow
