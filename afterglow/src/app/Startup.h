// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config/AppPaths.h"

#include <PhosphorWayland/LayerShellPluginLoader.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QString>

#include <utility>

namespace Afterglow {

/// Whether prepareApplication() set QT_WAYLAND_SHELL_INTEGRATION itself, for
/// restoreEnvironment().
inline bool& setShellIntegration()
{
    static bool set = false;
    return set;
}

/// What the process sets up before QApplication exists, for afterglow and
/// for the native window tests alike: the folder of its files, the plugin
/// path, the layer-shell integration (the quick terminal is a layer surface;
/// ordinary windows keep their xdg-shell role through it), and the names
/// Wayland and D-Bus see.
inline void prepareApplication()
{
    setAppDirectory();
    // registerLayerShellPlugin() also points Qt at the plugin beside the
    // embedded phosphor libraries, installed and in the build tree alike.
    const bool given = !qEnvironmentVariableIsEmpty("QT_WAYLAND_SHELL_INTEGRATION");
    PhosphorWayland::registerLayerShellPlugin();
    setShellIntegration() = !given && !qEnvironmentVariableIsEmpty("QT_WAYLAND_SHELL_INTEGRATION");
    QCoreApplication::setApplicationName(QStringLiteral("afterglow"));
    QCoreApplication::setApplicationVersion(QStringLiteral(AFTERGLOW_VERSION));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Afterglow"));
    QGuiApplication::setDesktopFileName(QStringLiteral(AFTERGLOW_APP_ID));
}

/// Call once QApplication exists. The variable that prepareApplication() set
/// is for Qt in this process: Qt reads it as it makes its platform
/// integration (QWaylandDisplay::initialize) and never again, also not when
/// it reconnects to a compositor. The programs of the panes, the sessions
/// and the scripts get the environment that afterglow got: a Qt program
/// started in a pane would ask for afterglow's shell integration plugin,
/// and one that links another PhosphorWayland than the plugin it finds
/// writes past the objects of that plugin. With
/// QT_WAYLAND_DONT_CHECK_SHELL_INTEGRATION, Qt reads it at the first window
/// instead, and it stays.
inline void restoreEnvironment()
{
    if (std::exchange(setShellIntegration(), false)
        && qEnvironmentVariableIntValue("QT_WAYLAND_DONT_CHECK_SHELL_INTEGRATION") == 0) {
        qunsetenv("QT_WAYLAND_SHELL_INTEGRATION");
    }
}

} // namespace Afterglow
