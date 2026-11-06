# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)
project(Afterglow VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(CTest)
include(GNUInstallDirs)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Zsh has no one place for completions: with the prefix /usr, Debian and
# Ubuntu read share/zsh/vendor-completions, not share/zsh/site-functions. The
# default is empty so that it follows CMAKE_INSTALL_DATADIR at each configure:
# a cached path would keep the DATADIR of the first one. _afterglow and _aglow
# both install here, also in a build of aglow alone.
set(AFTERGLOW_ZSH_COMPLETION_DIR "" CACHE STRING
    "Directory for the zsh completions, relative to the prefix or absolute (empty: DATADIR/zsh/site-functions)")
set(AFTERGLOW_ZSH_COMPLETION_INSTALL_DIR "${AFTERGLOW_ZSH_COMPLETION_DIR}")
if(AFTERGLOW_ZSH_COMPLETION_INSTALL_DIR STREQUAL "")
    set(AFTERGLOW_ZSH_COMPLETION_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/zsh/site-functions")
endif()

# aglow alone, for a computer that runs no Afterglow: a remote host, or a
# package of its own. It needs CMake, the C and C++20 compilers and the static
# C and C++ runtime libraries, but no Qt and no Phosphor checkout. zlib-ng and
# xxHash come from their pinned archives, as in the whole build. It installs
# aglow, its completions and its licence notices, nothing of the application.
option(AFTERGLOW_AGLOW_ONLY "Build only aglow and the protocol library that it uses: no Qt and no Phosphor libraries" OFF)
if(AFTERGLOW_AGLOW_ONLY)
    if(DEFINED AFTERGLOW_BUILD_AGLOW AND NOT AFTERGLOW_BUILD_AGLOW)
        message(FATAL_ERROR "AFTERGLOW_AGLOW_ONLY builds aglow. Turn AFTERGLOW_BUILD_AGLOW on, or AFTERGLOW_AGLOW_ONLY off.")
    endif()
    # The tools and the fuzz targets need the application's libraries, which
    # this build leaves out. Asked for, they would be missing without a word.
    foreach(option AFTERGLOW_BUILD_TOOLS AFTERGLOW_BUILD_FUZZERS)
        if(${option})
            message(FATAL_ERROR "AFTERGLOW_AGLOW_ONLY builds only aglow, not ${option}. Turn one of them off.")
        endif()
    endforeach()
    include(cmake/AfterglowDependencies.cmake)
    add_subdirectory(libs/afterglow-proto)
    add_subdirectory(tools/aglow)
    return()
endif()

# The phosphor libraries need Qt 6.10; they read QT_MIN_VERSION, so one floor
# applies to the whole build.
set(AFTERGLOW_QT_MIN 6.10)
set(QT_MIN_VERSION "${AFTERGLOW_QT_MIN}.0")
# Private APIs are required by the renderer and Wayland integration. Report
# their build-version requirement once instead of warning for each module.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 ${AFTERGLOW_QT_MIN} REQUIRED COMPONENTS Core Gui ShaderTools)
message(STATUS "Afterglow uses Qt ${Qt6_VERSION} private APIs; rebuild Afterglow after changing the Qt build.")
qt_standard_project_setup()

# Qt's signals/slots/emit keywords collide with ordinary identifiers (the
# phosphor libraries name variables `slots`); use Q_SIGNALS / Q_EMIT instead.
add_compile_definitions(QT_NO_KEYWORDS)

# Installed layout: the executable in bin/; afterglow's libraries, its own
# copies of the phosphor libraries and their layer-shell plugin in a private
# lib/afterglow/, so a system install of phosphor is never touched. The
# executable finds that directory; the libraries in it and the plugin set their
# own run paths where they are installed (afterglow/CMakeLists.txt).
set(AFTERGLOW_PRIVATE_LIBDIR "${CMAKE_INSTALL_LIBDIR}/afterglow")
set(CMAKE_INSTALL_RPATH "$ORIGIN/../${AFTERGLOW_PRIVATE_LIBDIR}")

include(cmake/AfterglowPhosphor.cmake)
# The pins of zlib-ng and xxHash (libs/afterglow-proto) and of the vendored
# stb_image (tools/aglow, checked by afterglow_vendored_file), after the
# Phosphor module so that a CPM which that module loaded is used again. Like
# it, this module is included at the top scope: CPM keeps its state in the
# variables of the scope that loads it.
include(cmake/AfterglowDependencies.cmake)

add_subdirectory(libs/afterglow-proto)  # protocol formats, terminal and client side: transfer, drag and drop, images, clipboard (no Qt)
add_subdirectory(libs/afterglow-vt)     # terminal emulation core: parser, screen, Unicode, input encoding (no Qt)
add_subdirectory(libs/afterglow-pty)    # pseudo-terminal spawn and I/O thread
add_subdirectory(libs/afterglow-render) # Vulkan (QRhi) grid renderer, font shaping, glyph atlas
option(AFTERGLOW_BUILD_TOOLS "Build developer tools: benchmarks, afterglow_renderpng and the headless conformance host" OFF)
option(AFTERGLOW_BUILD_AGLOW "Build aglow, the static client of the Kitty protocols and of script commands (no Qt). Without it, afterglow has no run, transfer, dnd, icat or clipboard command" ON)
option(AFTERGLOW_BUILD_FUZZERS "Build the libFuzzer targets for the terminal core, the protocol library and the image decoders (clang for C and C++)" OFF)
# A fuzz build instruments zlib-ng with the sanitizers (tools/fuzz), and a
# static aglow cannot link their runtimes. This normal variable hides the
# cached option in every directory, afterglow/ included.
if(AFTERGLOW_BUILD_FUZZERS AND AFTERGLOW_BUILD_AGLOW)
    message(STATUS "aglow: AFTERGLOW_BUILD_FUZZERS is on, so aglow is not built, and afterglow has no run, transfer, dnd, icat or clipboard command")
    set(AFTERGLOW_BUILD_AGLOW OFF)
endif()
add_subdirectory(afterglow)             # the application
if(AFTERGLOW_BUILD_AGLOW)
    add_subdirectory(tools/aglow)       # after the application: it goes next to its executable
endif()

if(AFTERGLOW_BUILD_TOOLS)
    add_subdirectory(tools/bench)
    add_subdirectory(tools/render)
    add_subdirectory(tools/conformance)
endif()
if(AFTERGLOW_BUILD_FUZZERS)
    add_subdirectory(tools/fuzz)
endif()
