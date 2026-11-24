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

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(CTest)
include(GNUInstallDirs)

# gtest_discover_tests lists each test executable's tests when CTest runs, not
# after the link. A listing at build time runs the test binary inside the build
# with a 5 s limit, which a loaded or niced build can miss and fail on, and it
# runs test binaries in builds that never test (packages, cross builds).
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# afterglow_sanitized(<variable>) sets <variable> ON when the build
# instruments its code with a sanitizer (the asan preset), else OFF. The
# flags of each build type count too, as the compiler and the linker get
# them. aglow then links dynamically (tools/aglow), and the decoding
# processes of aglow and aglow-mux keep standard error for the reports.
function(afterglow_sanitized variable)
    set(flags "${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
    foreach(config IN LISTS CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${config}" config)
        string(APPEND flags " ${CMAKE_CXX_FLAGS_${config}} ${CMAKE_C_FLAGS_${config}} ${CMAKE_EXE_LINKER_FLAGS_${config}}")
    endforeach()
    if(flags MATCHES "-fsanitize=")
        set(${variable} ON PARENT_SCOPE)
    else()
        set(${variable} OFF PARENT_SCOPE)
    endif()
endfunction()

# afterglow_test_runs(<test> <macro> <program>): the test executable <test>
# runs the <program> of this build, whose path the macro <macro> holds. A
# build of <test> alone builds <program> too.
function(afterglow_test_runs test macro program)
    target_compile_definitions(${test} PRIVATE ${macro}="$<TARGET_FILE:${program}>")
    add_dependencies(${test} ${program})
endfunction()

# afterglow_test_uses_jthread(<test>): the test executable <test> uses
# std::jthread. libc++ before LLVM 20, the base system's of FreeBSD 15, has
# it only with -fexperimental-library: the test gets that flag there.
function(afterglow_test_uses_jthread test)
    if(NOT DEFINED AFTERGLOW_JTHREAD_FLAG)
        include(CheckCXXSourceCompiles)
        set(source "#include <thread>\nint main() { std::jthread t([] {}); }")
        set(CMAKE_REQUIRED_FLAGS "-std=c++20 -pthread")
        set(CMAKE_REQUIRED_LINK_OPTIONS -pthread)
        check_cxx_source_compiles("${source}" AFTERGLOW_JTHREAD_PLAIN)
        set(flag "")
        if(NOT AFTERGLOW_JTHREAD_PLAIN)
            string(APPEND CMAKE_REQUIRED_FLAGS " -fexperimental-library")
            list(APPEND CMAKE_REQUIRED_LINK_OPTIONS -fexperimental-library)
            check_cxx_source_compiles("${source}" AFTERGLOW_JTHREAD_EXPERIMENTAL)
            if(AFTERGLOW_JTHREAD_EXPERIMENTAL)
                set(flag -fexperimental-library)
            endif()
        endif()
        set(AFTERGLOW_JTHREAD_FLAG "${flag}" CACHE INTERNAL "The flag that std::jthread needs, if any")
    endif()
    if(AFTERGLOW_JTHREAD_FLAG)
        target_compile_options(${test} PRIVATE ${AFTERGLOW_JTHREAD_FLAG})
        target_link_options(${test} PRIVATE ${AFTERGLOW_JTHREAD_FLAG})
    endif()
endfunction()

# Zsh has no one place for completions: with the prefix /usr, Debian and
# Ubuntu read share/zsh/vendor-completions, not share/zsh/site-functions. The
# default is empty so that it follows CMAKE_INSTALL_DATADIR at each configure:
# a cached path would keep the DATADIR of the first one. _afterglow and _aglow
# install here, and a build of aglow alone installs _aglow here too.
set(AFTERGLOW_ZSH_COMPLETION_DIR "" CACHE STRING
    "Directory for the zsh completions, relative to the prefix or absolute (empty: DATADIR/zsh/site-functions)")
set(AFTERGLOW_ZSH_COMPLETION_INSTALL_DIR "${AFTERGLOW_ZSH_COMPLETION_DIR}")
if(AFTERGLOW_ZSH_COMPLETION_INSTALL_DIR STREQUAL "")
    set(AFTERGLOW_ZSH_COMPLETION_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/zsh/site-functions")
endif()

# The installed layout, the private library directory and the run path are in
# cmake/AfterglowInstallDirs.cmake, which needs the suite folder's name from
# the phosphor-libs tree: cmake/AfterglowPhosphor.cmake includes it.
# The private libraries of aglow-mux, which it installs alone and afterglow
# installs with its own in a whole build. AglowOnlyCheck.cmake checks for
# them in the installation of aglow-mux alone.
set(AFTERGLOW_MUX_PRIVATE_LIBS AfterglowVt AfterglowPty PhosphorChannel)

# aglow and aglow-mux alone, for a computer that runs no Afterglow: a remote
# host, where the --ssh option of afterglow mux runs aglow-mux proxy, or
# packages of their own. It needs CMake, the C and C++20 compilers and the
# static C and C++ runtime libraries, but no Qt. zlib-ng, xxHash, the image
# codecs and Little CMS come from their pinned archives, as in the whole
# build. aglow-mux needs PCRE2, which pkg-config finds, and phosphor-channel,
# the one Phosphor library with no Qt, from the Phosphor checkout; without
# AFTERGLOW_BUILD_MUX the build makes only aglow and reads no checkout. It
# installs the two programs, the completions of aglow, the private libraries
# of aglow-mux and the license notices of both, nothing else of the
# application.
option(AFTERGLOW_AGLOW_ONLY "Build only aglow and aglow-mux, with the libraries that they use: no Qt, and of the Phosphor libraries only phosphor-channel. With AFTERGLOW_BUILD_MUX off, only aglow, with no Phosphor library" OFF)
# The whole build always builds aglow-mux, because afterglow runs it for
# afterglow mux: the option shows only with AFTERGLOW_AGLOW_ONLY.
include(CMakeDependentOption)
cmake_dependent_option(AFTERGLOW_BUILD_MUX "With AFTERGLOW_AGLOW_ONLY, build aglow-mux too. It needs PCRE2 (through pkg-config) and the Phosphor checkout" ON
                       "AFTERGLOW_AGLOW_ONLY" ON)
if(AFTERGLOW_AGLOW_ONLY)
    if(DEFINED AFTERGLOW_BUILD_AGLOW AND NOT AFTERGLOW_BUILD_AGLOW)
        message(FATAL_ERROR "AFTERGLOW_AGLOW_ONLY builds aglow. Turn AFTERGLOW_BUILD_AGLOW on, or AFTERGLOW_AGLOW_ONLY off.")
    endif()
    # The tools, the fuzz targets and the native window tests need the
    # application's libraries, which this build leaves out. Asked for, they
    # would be missing without a word.
    foreach(option AFTERGLOW_BUILD_TOOLS AFTERGLOW_BUILD_FUZZERS AFTERGLOW_BUILD_VTDIFF AFTERGLOW_NATIVE_TESTS)
        if(${option})
            message(FATAL_ERROR "AFTERGLOW_AGLOW_ONLY builds only aglow and aglow-mux, not ${option}. Turn one of them off.")
        endif()
    endforeach()
    include(cmake/AfterglowDependencies.cmake)
    if(AFTERGLOW_BUILD_MUX)
        include(cmake/AfterglowPhosphor.cmake)
    endif()
    add_subdirectory(libs/afterglow-proto)
    add_subdirectory(libs/afterglow-image)
    add_subdirectory(tools/aglow)
    if(AFTERGLOW_BUILD_MUX)
        add_subdirectory(libs/afterglow-vt)
        add_subdirectory(libs/afterglow-pty)
        add_subdirectory(mux)
    endif()
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
# It appends $ORIGIN and $ORIGIN/../lib to the run path, which
# cmake/AfterglowInstallDirs.cmake replaces after it.
qt_standard_project_setup()

# Qt's signals/slots/emit keywords collide with ordinary identifiers (the
# phosphor libraries name variables `slots`); use Q_SIGNALS / Q_EMIT instead.
add_compile_definitions(QT_NO_KEYWORDS)

# The pins of phosphor-libs, zlib-ng and xxHash (libs/afterglow-proto) and of
# the image codecs and Little CMS (libs/afterglow-image), and CPM. Included at
# the top scope: CPM keeps its state in the variables of the scope that loads
# it. Before the Phosphor module, which fetches the pinned phosphor-libs with
# it.
include(cmake/AfterglowDependencies.cmake)
include(cmake/AfterglowPhosphor.cmake)

option(AFTERGLOW_BUILD_TOOLS "Build developer tools: benchmarks, afterglow_renderpng and the headless conformance host" OFF)
option(AFTERGLOW_BUILD_AGLOW "Build aglow, the static client of the Kitty protocols and of script commands (no Qt). Without it, afterglow has no run, transfer, dnd, icat or clipboard command" ON)
option(AFTERGLOW_BUILD_FUZZERS "Build the libFuzzer targets for the terminal core, the protocol library, the image decoders and the attach stream of aglow-mux (clang for C and C++)" OFF)
option(AFTERGLOW_BUILD_VTDIFF "Build afterglow_vtdiff, which compares the terminal core with libghostty-vt (needs libghostty-vt in CMAKE_PREFIX_PATH)" OFF)
if(AFTERGLOW_BUILD_FUZZERS)
    # The instrumentation of the fuzz targets and of the third-party code
    # that they link (tools/fuzz). It is set before the libraries:
    # libs/afterglow-image hands it to the build of libjpeg-turbo, which
    # runs apart and takes no options afterwards.
    set(AFTERGLOW_FUZZ_FLAGS -g -O1 -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
    # A fuzz build instruments zlib-ng, the image codecs and Little CMS with
    # the sanitizers, and aglow links them statically: a static program
    # cannot link the sanitizer runtimes. This normal variable hides the
    # cached option in every directory, afterglow/ included.
    if(AFTERGLOW_BUILD_AGLOW)
        message(STATUS "aglow: AFTERGLOW_BUILD_FUZZERS is on, so aglow is not built, and afterglow has no run, transfer, dnd, icat or clipboard command")
        set(AFTERGLOW_BUILD_AGLOW OFF)
    endif()
endif()

add_subdirectory(libs/afterglow-proto)  # protocol formats, terminal and client side: transfer, drag and drop, images, clipboard, the phosphor-ipc client and server (no Qt)
add_subdirectory(libs/afterglow-vt)     # terminal emulation core: parser, screen, Unicode, input encoding (no Qt)
add_subdirectory(libs/afterglow-pty)    # pseudo-terminal spawn and I/O thread
add_subdirectory(libs/afterglow-image)  # image decoders of aglow and aglow-mux, with libpng, libjpeg-turbo, libwebp and Little CMS built in (no Qt)
add_subdirectory(libs/afterglow-render) # Vulkan (QRhi) grid renderer, font shaping, glyph atlas
add_subdirectory(mux)                   # aglow-mux, the session daemon, and the protocol that afterglow shares with it (no Qt)
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
if(AFTERGLOW_BUILD_VTDIFF)
    add_subdirectory(tools/vtdiff)
endif()
