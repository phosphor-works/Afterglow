# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pulls the phosphor libraries in from source: the phosphor-libs commit that
# cmake/AfterglowDependencies.cmake pins, fetched with CPM, or the tree that
# AFTERGLOW_PHOSPHOR_LIBS_DIR names, for a change to the libraries before the
# pin moves. Include it after that module.
#
# Each library is added in dependency order: a library that find_package()s a
# sibling picks up the in-tree target only if that target already exists.
#
# The build of aglow and aglow-mux alone (AFTERGLOW_AGLOW_ONLY) adds only
# phosphor-channel, which needs no Qt and no other library of the tree.

set(AFTERGLOW_PHOSPHOR_LIBS_DIR "" CACHE PATH
    "A phosphor-libs tree to build the Phosphor libraries from. Empty: the commit that cmake/AfterglowDependencies.cmake pins")

# A build of aglow alone reads the tree only for aglow-mux: each error about
# the tree says how to do without it.
set(AFTERGLOW_PHOSPHOR_WITHOUT "")
if(AFTERGLOW_AGLOW_ONLY)
    set(AFTERGLOW_PHOSPHOR_WITHOUT " Or turn AFTERGLOW_BUILD_MUX off, to build aglow without aglow-mux and phosphor-libs.")
endif()

# From here on AFTERGLOW_PHOSPHOR_LIBS_DIR names the tree in use, the pinned one
# when the cache entry is empty. afterglow/ and tools/aglow/ read it.
#
# A build that holds the whole phosphor-libs tree already, such as the workspace
# superbuild that adds it before every app, has its targets: Afterglow uses
# that tree and adds none of its libraries a second time.
# PHOSPHOR_LIBS_CMAKE_DIR is the cache entry the tree sets for its users.
set(AFTERGLOW_PHOSPHOR_LIBS_IN_BUILD OFF)
if(TARGET PhosphorChannel AND PHOSPHOR_LIBS_CMAKE_DIR)
    get_filename_component(AFTERGLOW_PHOSPHOR_LIBS_DIR "${PHOSPHOR_LIBS_CMAKE_DIR}" DIRECTORY)
    set(AFTERGLOW_PHOSPHOR_LIBS_IN_BUILD ON)
    message(STATUS "Afterglow: phosphor-libs is already in this build, at ${AFTERGLOW_PHOSPHOR_LIBS_DIR}")
elseif(AFTERGLOW_PHOSPHOR_LIBS_DIR)
    get_filename_component(AFTERGLOW_PHOSPHOR_LIBS_DIR "${AFTERGLOW_PHOSPHOR_LIBS_DIR}" ABSOLUTE)
else()
    afterglow_fetch_dependency(phosphor-libs)
    set(AFTERGLOW_PHOSPHOR_LIBS_DIR "${phosphor-libs_SOURCE_DIR}")
endif()

if(NOT IS_DIRECTORY "${AFTERGLOW_PHOSPHOR_LIBS_DIR}/libs")
    message(FATAL_ERROR
        "phosphor-libs not found at ${AFTERGLOW_PHOSPHOR_LIBS_DIR}. "
        "Set -DAFTERGLOW_PHOSPHOR_LIBS_DIR=/path/to/phosphor-libs, or leave it empty for the pinned commit.${AFTERGLOW_PHOSPHOR_WITHOUT}")
endif()

# The suite folder's name (PHOSPHOR_SUITE_DIRECTORY_NAME), which the install
# layout and the paths of the application are built from. A module with no
# dependency, so the aglow-only build reads it too.
if(NOT EXISTS "${AFTERGLOW_PHOSPHOR_LIBS_DIR}/cmake/PhosphorSuite.cmake")
    message(FATAL_ERROR
        "${AFTERGLOW_PHOSPHOR_LIBS_DIR}/cmake/PhosphorSuite.cmake not found. "
        "This phosphor-libs tree is older than the commit that cmake/AfterglowDependencies.cmake pins.")
endif()
include("${AFTERGLOW_PHOSPHOR_LIBS_DIR}/cmake/PhosphorSuite.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AfterglowInstallDirs.cmake")

# The phosphor libraries' install rules are for a system install of phosphor
# (headers, CMake packages, the plugin in Qt's directory). Afterglow installs
# the parts it uses privately instead, so they are skipped here. Not in a build
# that holds the tree already: its install rules are that build's to keep, and
# install() is a global command that every later project would go through.
if(NOT AFTERGLOW_PHOSPHOR_LIBS_IN_BUILD)
    macro(install)
        if(NOT AFTERGLOW_INSIDE_PHOSPHOR)
            _install(${ARGN})
        endif()
    endmacro()
endif()

function(afterglow_add_phosphor_libs)
    if(AFTERGLOW_PHOSPHOR_LIBS_IN_BUILD)
        return()
    endif()
    # The phosphor libraries' own test suites are not part of this build.
    set(BUILD_TESTING OFF)
    set(AFTERGLOW_INSIDE_PHOSPHOR ON)
    foreach(lib IN LISTS ARGN)
        # A checkout older than a library that Afterglow needs: say which
        # commit to take, not only that a directory is missing.
        if(NOT EXISTS "${AFTERGLOW_PHOSPHOR_LIBS_DIR}/libs/${lib}/CMakeLists.txt")
            message(FATAL_ERROR
                "${AFTERGLOW_PHOSPHOR_LIBS_DIR} has no ${lib}. This phosphor-libs tree is older than the commit "
                "that cmake/AfterglowDependencies.cmake pins.${AFTERGLOW_PHOSPHOR_WITHOUT}")
        endif()
        add_subdirectory("${AFTERGLOW_PHOSPHOR_LIBS_DIR}/libs/${lib}"
                         "${CMAKE_BINARY_DIR}/phosphor/${lib}" EXCLUDE_FROM_ALL)
    endforeach()
endfunction()

if(AFTERGLOW_AGLOW_ONLY)
    afterglow_add_phosphor_libs(phosphor-channel)
    return()
endif()

# The tree already in the build brought its own pins with it.
if(NOT AFTERGLOW_PHOSPHOR_LIBS_IN_BUILD)
    # The script mode of cmake/AfterglowDependencies.cmake puts the archives of
    # both pin files into one directory. An offline build that names it as
    # AFTERGLOW_DEPS_ARCHIVES then needs no second option for the archives of the
    # libraries.
    if(AFTERGLOW_DEPS_ARCHIVES AND NOT PHOSPHOR_DEPS_ARCHIVES)
        set(PHOSPHOR_DEPS_ARCHIVES "${AFTERGLOW_DEPS_ARCHIVES}" CACHE PATH
            "Directory with the archives that the phosphor-libs tree pins, for an offline build" FORCE)
    endif()

    # The pins of the archives that the libraries fetch (Luau, jsoncons). The module
    # must be included at the top scope: its PHOSPHOR_DEPS list is a directory
    # variable, and a library that included it itself would hide the list from its
    # siblings. It loads no second CPM: cmake/AfterglowDependencies.cmake has
    # loaded one. An older tree has no such module, and include() alone would then
    # fail without saying why.
    if(NOT EXISTS "${AFTERGLOW_PHOSPHOR_LIBS_DIR}/cmake/PhosphorDependencies.cmake")
        message(FATAL_ERROR
            "${AFTERGLOW_PHOSPHOR_LIBS_DIR}/cmake/PhosphorDependencies.cmake not found. "
            "This phosphor-libs tree is older than the commit that cmake/AfterglowDependencies.cmake pins.")
    endif()
    include("${AFTERGLOW_PHOSPHOR_LIBS_DIR}/cmake/PhosphorDependencies.cmake")
endif()

afterglow_add_phosphor_libs(
    phosphor-paths
    phosphor-audio
    phosphor-channel
    phosphor-dbus
    phosphor-ipc
    phosphor-scripting
    phosphor-config
    phosphor-fsloader
    phosphor-mcp
    phosphor-registry
    phosphor-shaders
    phosphor-rendering
    phosphor-wayland
    phosphor-layer
    phosphor-animation
    phosphor-shortcuts
)
