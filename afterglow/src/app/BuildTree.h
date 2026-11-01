// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFileInfo>
#include <QString>

namespace Afterglow {

/// Whether this process runs an executable in the build tree. Files there
/// (the QPA plugin, the terminfo entry, the checkout's packs, themes and
/// shell integration) are for that build alone: an installed binary must not
/// load them because a build directory exists. Both paths are canonical, so
/// a build directory reached through a symbolic link still counts.
/// Its own header, without Startup.h's Phosphor includes, so that the
/// shader, config and shell libraries can use it.
inline bool runningFromBuildTree()
{
#ifdef AFTERGLOW_BUILD_DIR
    static const bool inside = [] {
        const QString build = QFileInfo(QStringLiteral(AFTERGLOW_BUILD_DIR)).canonicalFilePath();
        const QString self = QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
        return !build.isEmpty() && self.startsWith(build + QLatin1Char('/'));
    }();
    return inside;
#else
    return false;
#endif
}

} // namespace Afterglow
