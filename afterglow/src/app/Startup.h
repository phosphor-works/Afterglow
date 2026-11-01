// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "BuildTree.h"

#include <PhosphorWayland/LayerShellPluginLoader.h>

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QString>

namespace Afterglow {

/// What the process sets up before QApplication exists, for afterglow and
/// for the native window tests alike: the plugin paths, the layer-shell
/// integration (the quick terminal is a layer surface; ordinary windows keep
/// their xdg-shell role through it), and the names Wayland and D-Bus see.
inline void prepareApplication()
{
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
    PhosphorWayland::registerLayerShellPlugin();
    QCoreApplication::setApplicationName(QStringLiteral("afterglow"));
    QCoreApplication::setApplicationVersion(QStringLiteral(AFTERGLOW_VERSION));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Afterglow"));
    QGuiApplication::setDesktopFileName(QStringLiteral(AFTERGLOW_APP_ID));
}

} // namespace Afterglow
