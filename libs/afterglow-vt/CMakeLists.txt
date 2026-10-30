# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

add_library(AfterglowVt SHARED
    src/Clipboard.cpp
    src/Graphics.cpp
    src/ITerm2.cpp
    src/ShellContext.cpp
    src/Input.cpp
    src/Notifications.cpp
    src/ProgramStatus.cpp
    src/Screen.cpp
    src/TextSearch.cpp
    src/Search.cpp
    src/Selection.cpp
    src/Sixel.cpp
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
find_package(PkgConfig REQUIRED)
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-32)
find_package(ZLIB REQUIRED)
target_link_libraries(AfterglowVt PRIVATE ZLIB::ZLIB PkgConfig::PCRE2)

# The parser's hot path crosses files and the library's own exported
# functions: let calls inside the library go straight to their targets (no
# PLT, nothing interposes them) and be inlined across files.
target_compile_options(AfterglowVt PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-semantic-interposition>)
include(CheckIPOSupported)
check_ipo_supported(RESULT AFTERGLOW_VT_IPO LANGUAGES CXX)
if(AFTERGLOW_VT_IPO)
    set_target_properties(AfterglowVt PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()

set_target_properties(AfterglowVt PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    AUTOMOC OFF)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
