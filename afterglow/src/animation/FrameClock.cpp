// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FrameClock.h"

#include <QScreen>
#include <QWindow>

namespace Afterglow {

std::chrono::nanoseconds FrameClock::now() const
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

qreal FrameClock::refreshRate() const
{
    const QScreen* screen = m_window->screen();
    return screen ? screen->refreshRate() : 60.0;
}

void FrameClock::requestFrame()
{
    m_window->requestUpdate();
}

} // namespace Afterglow
