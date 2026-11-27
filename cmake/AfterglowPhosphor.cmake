# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Adds the Phosphor libraries, embedded in Afterglow
# (afterglow_add_phosphor_libs in cmake/AfterglowDependencies.cmake), and sets
# up the installed layout from the directories they install in. A build of aglow
# and aglow-mux alone has no Qt: it asks the tree for the libraries that do not
# use Qt, which aglow-mux needs (phosphor-channel).

if(AFTERGLOW_AGLOW_ONLY)
    set(PHOSPHOR_LIBS_QT OFF)
endif()
afterglow_add_phosphor_libs()
# The suite folder name and the install functions, in Afterglow's own scope:
# the tree includes the module only in its own.
include("${PHOSPHOR_LIBS_CMAKE_DIR}/PhosphorSuite.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AfterglowInstallDirs.cmake")
