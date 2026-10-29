# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

add_library(AfterglowVt SHARED
    src/Clipboard.cpp
    src/Graphics.cpp
    src/Input.cpp
    src/Notifications.cpp
    src/ProgramStatus.cpp
    src/Screen.cpp
    src/Search.cpp
    src/Selection.cpp
    src/Style.cpp
    src/Terminal.cpp
    src/Unicode.cpp
)
add_library(Afterglow::Vt ALIAS AfterglowVt)

include(GenerateExportHeader)
generate_export_header(AfterglowVt
    BASE_NAME AfterglowVt
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/AfterglowVt/afterglowvt_export.h)

target_include_directories(AfterglowVt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
find_package(ZLIB REQUIRED)
target_link_libraries(AfterglowVt PRIVATE ZLIB::ZLIB)

set_target_properties(AfterglowVt PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    AUTOMOC OFF)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
