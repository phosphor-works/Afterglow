# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libFuzzer target for the terminal core (clang only). The core's sources are
# compiled into it directly so the sanitizers instrument them.
#
#   cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DAFTERGLOW_BUILD_FUZZERS=ON
#   cmake --build build-fuzz --target afterglow_vtfuzz
#   mkdir -p corpus
#   ./build-fuzz/tools/fuzz/afterglow_vtfuzz -max_total_time=300 -max_len=65536 \
#       -dict=tools/fuzz/vt.dict corpus/

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AFTERGLOW_BUILD_FUZZERS needs clang (libFuzzer)")
endif()
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-32)
file(GLOB VT_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/libs/afterglow-vt/src/*.cpp)
add_executable(afterglow_vtfuzz vtfuzz.cpp ${VT_SOURCES})
target_include_directories(afterglow_vtfuzz PRIVATE
    ${PROJECT_SOURCE_DIR}/libs/afterglow-vt/include
    ${PROJECT_BINARY_DIR}/libs/afterglow-vt/include)
target_compile_options(afterglow_vtfuzz PRIVATE -g -O1 -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined)
target_link_options(afterglow_vtfuzz PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_libraries(afterglow_vtfuzz PRIVATE ZLIB::ZLIB PkgConfig::PCRE2)
set_target_properties(afterglow_vtfuzz PROPERTIES AUTOMOC OFF)
