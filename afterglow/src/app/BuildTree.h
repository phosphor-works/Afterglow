// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFileInfo>
#include <QString>

namespace Afterglow {

// Every target that includes this header defines the macro: an inline body
// that differed between translation units would break the one-definition rule.
#ifndef AFTERGLOW_BUILD_DIR
#error "AFTERGLOW_BUILD_DIR must be defined for every target that includes BuildTree.h"
#endif

/// Whether this process runs an executable in the build tree. Files there
/// (the QPA plugin, the terminfo entry, the checkout's packs, themes and
/// shell integration) are for that build alone: an installed binary must not
/// load them because a build directory exists. Both paths are canonical, so
/// a build directory reached through a symbolic link still counts.
/// Its own header, without Startup.h's Phosphor includes, so that the
/// shader, config and shell libraries can use it.
inline bool runningFromBuildTree()
{
    static const bool inside = [] {
        const QString build = QFileInfo(QStringLiteral(AFTERGLOW_BUILD_DIR)).canonicalFilePath();
        const QString self = QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
        return !build.isEmpty() && self.startsWith(build + QLatin1Char('/'));
    }();
    return inside;
}

} // namespace Afterglow
