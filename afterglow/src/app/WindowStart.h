// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../config/Settings.h"

#include <QWindow>

namespace Afterglow {

/// Shows a new window as Window.startMode asks, whatever made it: a launch,
/// `afterglow ctl new-window`, a script, or a pane or tab moved out.
/// Window.decorations must be set before, as the first show reads it.
inline void showForStartMode(QWindow& window, const WindowSettings& settings)
{
    switch (settings.startMode) {
    case WindowSettings::StartMode::Maximized:
        window.showMaximized();
        break;
    case WindowSettings::StartMode::Fullscreen:
        window.showFullScreen();
        break;
    case WindowSettings::StartMode::Normal:
        window.show();
        break;
    }
}

} // namespace Afterglow
