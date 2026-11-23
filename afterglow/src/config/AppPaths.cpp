// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/AppPaths.h"

#include <PhosphorPaths/SuiteDirectories.h>

namespace Afterglow {

// Both names come from cmake/AfterglowInstallDirs.cmake, which installs the
// files there too.
void setAppDirectory()
{
    PhosphorPaths::setAppDirectoryName(QStringLiteral(AFTERGLOW_APP_DIRECTORY_NAME));
}

QString shaderDataSubdir()
{
    return PhosphorPaths::commonDataSubdir(QStringLiteral(AFTERGLOW_SHADER_TREE));
}

} // namespace Afterglow
