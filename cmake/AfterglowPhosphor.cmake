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

function(afterglow_add_phosphor_libs)
    # The phosphor libraries' own test suites are not part of this build.
    set(BUILD_TESTING OFF)
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
)
