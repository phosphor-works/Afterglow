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
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(CTest)
include(GNUInstallDirs)

set(AFTERGLOW_QT_MIN 6.8)
find_package(Qt6 ${AFTERGLOW_QT_MIN} REQUIRED COMPONENTS Core Gui ShaderTools)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

include(cmake/AfterglowPhosphor.cmake)

add_subdirectory(libs/afterglow-vt)     # terminal emulation core: parser, screen, Unicode, input encoding (no Qt)
add_subdirectory(libs/afterglow-pty)    # pseudo-terminal spawn and I/O thread
add_subdirectory(libs/afterglow-render) # Vulkan (QRhi) grid renderer, font shaping, glyph atlas
add_subdirectory(afterglow)             # the application
