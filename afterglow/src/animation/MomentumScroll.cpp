// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MomentumScroll.h"

#include <algorithm>
#include <cmath>

namespace Afterglow {

namespace {
// kitty's values (glfw/momentum-scroll.c): the speed counts the last 150 ms
// of a gesture, steps are capped at 100 pixels and end below half a pixel,
// and a gesture of 10 pixels or less does not coast.
constexpr std::int64_t SampleWindowMs = 150;
constexpr double MaxSpeed = 100;
constexpr double MinSpeed = 0.5;
constexpr double MinDisplacement = 10;
} // namespace

void MomentumScroll::setFriction(double friction)
{
    m_friction = std::isfinite(friction) ? std::clamp(friction, 0.0, 1.0) : 0.96;
    if (m_friction == 0) cancel();
}

void MomentumScroll::addSpeed(double speed)
{
    // Speed in the same direction adds up; the other way starts again.
    m_speed = speed == 0 || speed * m_speed >= 0 ? m_speed + speed : speed;
    m_speed = std::clamp(m_speed, -MaxSpeed, MaxSpeed);
}

void MomentumScroll::move(double delta, std::int64_t ms)
{
    if (m_coasting) {
        m_coasting = false;
        // A finger against the coast stops it dead; one with it keeps the
        // speed, so repeated flicks go faster.
        if (delta * m_speed < 0) m_speed = 0;
    }
    if (!m_gesture) {
        m_gesture = true;
        m_gestureStart = ms;
        m_displacement = 0;
    }
    if (!m_samples.empty() && m_samples.back().delta * delta < 0) {
        m_samples.clear();
        m_speed = 0;
    }
    m_displacement += delta;
    m_samples.push_back({delta, ms});
}

bool MomentumScroll::release(std::int64_t ms)
{
    const bool flick = m_gesture && std::abs(m_displacement) > MinDisplacement && ms - m_gestureStart > 2;
    m_gesture = false;
    while (!m_samples.empty() && ms - m_samples.front().ms > SampleWindowMs) m_samples.pop_front();
    if (!flick || m_friction == 0 || m_samples.empty()) {
        m_samples.clear();
        m_speed = 0;
        return false;
    }
    // A weighted mean of the deltas: the last sample counts twice as much
    // as the first.
    const std::int64_t first = m_samples.front().ms;
    const double span = double(std::max<std::int64_t>(1, m_samples.back().ms - first));
    double total = 0, weights = 0;
    for (const Sample& s : m_samples) {
        const double weight = 1.0 + double(s.ms - first) / span;
        total += s.delta * weight;
        weights += weight;
    }
    m_samples.clear();
    addSpeed(total / weights);
    m_coasting = std::abs(m_speed) >= MinSpeed;
    m_nextStep = ms;
    return m_coasting;
}

void MomentumScroll::cancel()
{
    m_coasting = false;
    m_gesture = false;
    m_samples.clear();
    m_speed = 0;
}

double MomentumScroll::advance(std::int64_t ms)
{
    double distance = 0;
    while (m_coasting && ms >= m_nextStep) {
        m_speed *= m_friction;
        if (std::abs(m_speed) < MinSpeed) {
            m_speed = 0;
            m_coasting = false;
            break;
        }
        distance += m_speed;
        m_nextStep += StepMs;
    }
    return distance;
}

} // namespace Afterglow
