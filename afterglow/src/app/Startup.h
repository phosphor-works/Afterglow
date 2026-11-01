// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorWayland/LayerShellPluginLoader.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QString>

namespace Afterglow {

/// Whether this process runs the executable in the build tree. Files there
/// (the QPA plugin, the terminfo entry) are for that build alone: an
/// installed binary must not load them because a build directory exists.
inline bool runningFromBuildTree()
{
    static const bool inside = [] {
        const QString build = QFileInfo(QStringLiteral(AFTERGLOW_BUILD_DIR)).canonicalFilePath();
        const QString self = QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
        return !build.isEmpty() && self.startsWith(build + QLatin1Char('/'));
    }();
    return inside;
}

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
