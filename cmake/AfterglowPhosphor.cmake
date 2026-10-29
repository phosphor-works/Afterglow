# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pulls the phosphor libraries in from a source checkout. Until phosphor-libs
# lives in its own repository this points at the PlasmaZones tree next to this
# one; override AFTERGLOW_PHOSPHOR_LIBS_DIR to use a different checkout.
#
# Each library is added in dependency order: a library that find_package()s a
# sibling picks up the in-tree target only if that target already exists.

set(AFTERGLOW_PHOSPHOR_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../PlasmaZones/phosphor-libs"
    CACHE PATH "Path to the phosphor-libs directory of a PlasmaZones checkout")

if(NOT EXISTS "${AFTERGLOW_PHOSPHOR_LIBS_DIR}/libs/phosphor-rendering/CMakeLists.txt")
    message(FATAL_ERROR
        "phosphor-libs not found at ${AFTERGLOW_PHOSPHOR_LIBS_DIR}. "
        "Set -DAFTERGLOW_PHOSPHOR_LIBS_DIR=/path/to/PlasmaZones/phosphor-libs.")
endif()

# The phosphor libraries' install rules are for a system install of phosphor
# (headers, CMake packages, the plugin in Qt's directory). Afterglow installs
# the parts it uses privately instead, so they are skipped here.
macro(install)
    if(NOT AFTERGLOW_INSIDE_PHOSPHOR)
        _install(${ARGN})
    endif()
endmacro()

function(afterglow_add_phosphor_libs)
    # The phosphor libraries' own test suites are not part of this build.
    set(BUILD_TESTING OFF)
    set(AFTERGLOW_INSIDE_PHOSPHOR ON)
    foreach(lib IN LISTS ARGN)
        add_subdirectory("${AFTERGLOW_PHOSPHOR_LIBS_DIR}/libs/${lib}"
                         "${CMAKE_BINARY_DIR}/phosphor/${lib}" EXCLUDE_FROM_ALL)
    endforeach()
endfunction()

afterglow_add_phosphor_libs(
    phosphor-config
    phosphor-fsloader
    phosphor-registry
    phosphor-shaders
    phosphor-rendering
    phosphor-wayland
    phosphor-layer
    phosphor-animation
    phosphor-shortcuts
)
