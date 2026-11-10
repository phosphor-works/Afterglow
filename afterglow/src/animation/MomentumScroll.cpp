// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MomentumScroll.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Afterglow {

namespace {
// kitty's values (glfw/momentum-scroll.c): the speed counts the last 150 ms
// of a gesture, it is capped at 100 pixels for each step and ends below
// half a pixel, and a gesture of 10 pixels or less, or of 2 ms or less,
// does not coast.
constexpr double SampleWindowMs = 150;
constexpr double MaxSpeed = 100;
constexpr double MinSpeed = 0.5;
constexpr double MinDisplacement = 10;
constexpr double MinDurationMs = 2;
// A frame that comes late (the window was busy) goes on from where the
// coast was, as kitty's timer only delays its steps: the coast moves by no
// more than this much of its own time between two frames.
constexpr double MaxGapMs = 50;
} // namespace

void MomentumScroll::setFriction(double friction)
{
    if (!std::isfinite(friction)) return;
    friction = std::clamp(friction, 0.0, 1.0);
    if (friction == m_friction) return;
    const double speed = m_coasting ? coastSpeed() : 0;
    m_friction = friction;
    if (m_friction == 0) {
        cancel();
    } else if (m_coasting) {
        startCoast(speed);
    }
}

void MomentumScroll::addSpeed(double speed)
{
    // Speed in the same direction adds up, and the other way starts again.
    m_speed = speed == 0 || speed * m_speed >= 0 ? m_speed + speed : speed;
    m_speed = std::clamp(m_speed, -MaxSpeed, MaxSpeed);
}

double MomentumScroll::coastSpeed() const
{
    return m_coastStart * std::pow(m_friction, std::min(m_coastMs / StepMs, m_coastSteps));
}

void MomentumScroll::startCoast(double speed)
{
    m_coastStart = speed;
    m_coastMs = 0;
    m_travelled = 0;
    // The steps whose speed, slowed by the friction, is still half a pixel
    // or more: kitty ends the coast at the first one that is not.
    if (std::abs(speed) * m_friction < MinSpeed) {
        m_coastSteps = 0;
    } else if (m_friction >= 1) {
        m_coastSteps = std::numeric_limits<double>::infinity();
    } else {
        m_coastSteps = std::floor(std::log(MinSpeed / std::abs(speed)) / std::log(m_friction));
    }
    m_coasting = m_coastSteps >= 1;
}

void MomentumScroll::trimSamples(double ms)
{
    while (!m_samples.empty() && ms - m_samples.front().ms > SampleWindowMs) m_samples.pop_front();
}

void MomentumScroll::begin(double ms)
{
    if (m_coasting) {
        m_speed = coastSpeed();
        m_coasting = false;
    }
    m_gesture = true;
    m_gestureStart = ms;
    m_displacement = 0;
    m_samples.clear();
}

void MomentumScroll::move(double delta, double ms)
{
    if (!m_gesture) begin(ms);
    if (m_coasting) {
        m_speed = coastSpeed();
        m_coasting = false;
    }
    if (!m_samples.empty() && m_samples.back().delta * delta < 0) {
        m_samples.clear();
        m_speed = 0;
    }
    m_displacement += delta;
    m_samples.push_back({delta, ms});
    trimSamples(ms);
}

bool MomentumScroll::release(double ms)
{
    const bool flick = m_gesture && std::abs(m_displacement) > MinDisplacement && ms - m_gestureStart > MinDurationMs;
    m_gesture = false;
    m_coasting = false;
    // As kitty, the lift is a sample too, with no distance and the most
    // weight: a finger that stopped before it lifted coasts slower.
    m_samples.push_back({0, ms});
    trimSamples(ms);
    if (!flick || m_friction == 0) {
        m_samples.clear();
        m_speed = 0;
        return false;
    }
    // A weighted mean of the deltas: the last sample counts twice as much
    // as the first.
    const double first = m_samples.front().ms;
    const double span = std::max(1.0, m_samples.back().ms - first);
    double total = 0, weights = 0;
    for (const Sample& s : m_samples) {
        const double weight = 1.0 + (s.ms - first) / span;
        total += s.delta * weight;
        weights += weight;
    }
    m_samples.clear();
    addSpeed(total / weights);
    m_coastLastMs = ms;
    startCoast(m_speed);
    if (!m_coasting) m_speed = 0;
    return m_coasting;
}

void MomentumScroll::cancel()
{
    m_coasting = false;
    m_gesture = false;
    m_samples.clear();
    m_speed = 0;
}

double MomentumScroll::advance(double ms)
{
    if (!m_coasting) return 0;
    m_coastMs += std::clamp(ms - m_coastLastMs, 0.0, MaxGapMs);
    m_coastLastMs = std::max(m_coastLastMs, ms);
    double steps = m_coastMs / StepMs;
    if (steps >= m_coastSteps) {
        steps = m_coastSteps;
        m_coasting = false;
        m_speed = 0;
    }
    // Where kitty's steps put the coast after a whole number of them, and
    // between two of them in proportion: the speed after n steps is
    // v * f^n, and the distance is their sum, v * f * (1 - f^n) / (1 - f).
    const double at = m_friction >= 1 ? m_coastStart * steps
                                      : m_coastStart * m_friction * (1 - std::pow(m_friction, steps)) / (1 - m_friction);
    const double distance = at - m_travelled;
    m_travelled = at;
    return distance;
}

} // namespace Afterglow
