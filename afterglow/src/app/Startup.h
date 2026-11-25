// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "BuildTree.h"
#include "config/AppPaths.h"

#include <PhosphorWayland/LayerShellPluginLoader.h>

#include <QCoreApplication>
#include <QDir>
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
/// paths, the layer-shell integration (the quick terminal is a layer surface;
/// ordinary windows keep their xdg-shell role through it), and the names
/// Wayland and D-Bus see.
inline void prepareApplication()
{
    setAppDirectory();
    // The plugin lives in afterglow's private directory when installed, and
    // in the build tree for the build's own binaries (added last, it wins).
#ifdef AFTERGLOW_INSTALLED_PLUGIN_DIR
    if (QDir(QStringLiteral(AFTERGLOW_INSTALLED_PLUGIN_DIR)).exists()) {
        QCoreApplication::addLibraryPath(QStringLiteral(AFTERGLOW_INSTALLED_PLUGIN_DIR));
    }
#endif
#ifdef AFTERGLOW_BUILD_PLUGIN_DIR
    if (runningFromBuildTree() && QDir(QStringLiteral(AFTERGLOW_BUILD_PLUGIN_DIR)).exists()) {
        QCoreApplication::addLibraryPath(QStringLiteral(AFTERGLOW_BUILD_PLUGIN_DIR));
    }
#endif
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
