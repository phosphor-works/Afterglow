# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

add_library(AfterglowVt SHARED
    src/Graphics.cpp
    src/ITerm2.cpp
    src/ShellContext.cpp
    src/Input.cpp
    src/KeyNames.cpp
    src/Notifications.cpp
    src/ProgramStatus.cpp
    src/Screen.cpp
    src/TextSearch.cpp
    src/Search.cpp
    src/Selection.cpp
    src/Sixel.cpp
    src/StateSync.cpp
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
# The wire formats of the protocols the terminal parses: images, clipboard,
# base64, the text checks, the command limits of drag and drop and file
# transfer, and the remote socket query of aglow. PUBLIC, because the public
# headers include them: Terminal.h holds a ClipboardAssembler from
# AfterglowProto/Clipboard.h and its host answers the OSC 24569 query with a
# Remote::Answer from AfterglowProto/RemoteOsc.h, and Graphics.h keeps the
# pending transmission's GraphicsCommand from AfterglowProto/Graphics.h.
target_link_libraries(AfterglowVt PUBLIC Afterglow::Proto PRIVATE PkgConfig::PCRE2)
# XTVERSION reports the project's version. A generated header, not a compile
# definition, so tools/fuzz, which compiles these sources itself, has it too.
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/include/AfterglowVt/afterglowvt_version.h
     CONTENT "#pragma once\n#define AFTERGLOW_VT_VERSION \"@PROJECT_VERSION@\"\n" @ONLY)

# The parser's hot path crosses files and the library's own exported
# functions: let calls inside the library go straight to their targets (no
# PLT, nothing interposes them) and be inlined across files.
target_compile_options(AfterglowVt PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-semantic-interposition>)
# CMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF turns it off here too, as for every
# other target. The aglow_only_build test does so: GCC's -flto=auto runs a job
# for each processor under a build tool that offers no jobserver, beyond the
# jobs that ctest counts for the test.
include(CheckIPOSupported)
check_ipo_supported(RESULT AFTERGLOW_VT_IPO LANGUAGES CXX)
if(DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION AND NOT CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    set(AFTERGLOW_VT_IPO OFF)
endif()
if(AFTERGLOW_VT_IPO)
    set_target_properties(AfterglowVt PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()

set_target_properties(AfterglowVt PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    AUTOMOC OFF
    AUTOUIC OFF)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
