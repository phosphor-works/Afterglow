// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// Names Afterglow's folder before main(), as afterglow does at the top of its
// own: the tests read their paths as the application does, and PhosphorPaths
// aborts on a path read before the folder has a name. Linked into every test
// executable of this directory (CMakeLists.txt).

#include "config/AppPaths.h"

namespace {
const bool appDirectorySet = [] {
    Afterglow::setAppDirectory();
    return true;
}();
} // namespace
