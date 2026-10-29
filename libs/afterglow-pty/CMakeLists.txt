# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

add_library(AfterglowPty SHARED src/Pty.cpp)
add_library(Afterglow::Pty ALIAS AfterglowPty)

include(GenerateExportHeader)
generate_export_header(AfterglowPty
    BASE_NAME AfterglowPty
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/AfterglowPty/afterglowpty_export.h)

find_package(Threads REQUIRED)
target_link_libraries(AfterglowPty PRIVATE Threads::Threads util)
target_include_directories(AfterglowPty PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
set_target_properties(AfterglowPty PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    AUTOMOC OFF)
