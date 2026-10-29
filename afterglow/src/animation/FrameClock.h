// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorAnimation/IMotionClock.h>

class QWindow;

namespace Afterglow {

/// Drives phosphor animations from a window's frame loop: a frame request is
/// a QWindow update request, so animations tick at the display's pace.
class FrameClock final : public PhosphorAnimation::IMotionClock
{
public:
    explicit FrameClock(QWindow* window)
        : m_window(window)
    {
    }

    std::chrono::nanoseconds now() const override;
    qreal refreshRate() const override;
    void requestFrame() override;
    const void* epochIdentity() const override { return steadyClockEpoch(); }

private:
    QWindow* m_window;
};

} // namespace Afterglow
