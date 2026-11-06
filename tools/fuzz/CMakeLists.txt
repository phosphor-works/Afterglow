# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libFuzzer targets (clang only, for C and C++): afterglow_vtfuzz for the
# terminal core, afterglow_protofuzz for the protocol readers that take what
# a remote program sends, and afterglow_imagefuzz for the image decoders of
# aglow (the patched stb_image and aglow's GIF reader in tools/aglow). The
# sources of the libraries, xxHash and the decoders are compiled into them
# again, so that the sanitizers and the coverage instrument them. zlib-ng is
# instrumented in place: its own build sets CPU flags for each of its files,
# so its source list cannot be compiled again here. Every binary in a fuzz
# build directory that links zlib-ng then needs the sanitizer runtimes. Only
# the fuzz targets are built and supported there, so the configure turns the
# tests off, CMakeLists.txt turns aglow off (a static aglow cannot link the
# runtimes), and the build names its targets.
#
#   cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DAFTERGLOW_BUILD_FUZZERS=ON \
#       -DBUILD_TESTING=OFF
#   cmake --build build-fuzz --target afterglow_vtfuzz afterglow_protofuzz afterglow_imagefuzz
#   mkdir -p build-fuzz/corpus
#   ./build-fuzz/tools/fuzz/afterglow_vtfuzz -max_total_time=300 -max_len=65536 \
#       -dict=tools/fuzz/vt.dict build-fuzz/corpus/

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AFTERGLOW_BUILD_FUZZERS needs clang for C and C++ (libFuzzer): "
        "-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-32)

set(AFTERGLOW_FUZZ_FLAGS -g -O1 -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)

# Sets OUT to the sources of TARGET with absolute paths, so the fuzz targets
# compile what the library compiles.
function(afterglow_fuzz_sources target out)
    get_target_property(sources ${target} SOURCES)
    get_target_property(directory ${target} SOURCE_DIR)
    list(TRANSFORM sources PREPEND "${directory}/" REGEX "^[^/$]")
    set(${out} ${sources} PARENT_SCOPE)
endfunction()

target_compile_options(zlib-ng PRIVATE ${AFTERGLOW_FUZZ_FLAGS})
target_link_options(zlib-ng INTERFACE -fsanitize=address,undefined)

# The protocol library with xxHash, for both targets: the terminal core calls
# it too.
afterglow_fuzz_sources(AfterglowProto PROTO_SOURCES)
afterglow_fuzz_sources(afterglow_xxhash XXHASH_SOURCES)
add_library(afterglow_fuzz_proto STATIC ${PROTO_SOURCES} ${XXHASH_SOURCES})
target_include_directories(afterglow_fuzz_proto PUBLIC $<TARGET_PROPERTY:AfterglowProto,INTERFACE_INCLUDE_DIRECTORIES>)
target_include_directories(afterglow_fuzz_proto SYSTEM PRIVATE
    $<TARGET_PROPERTY:afterglow_xxhash,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(afterglow_fuzz_proto PUBLIC afterglow_zlib)
target_compile_options(afterglow_fuzz_proto PRIVATE ${AFTERGLOW_FUZZ_FLAGS})
# Upstream code is not ours to fix (as in libs/afterglow-proto).
set_source_files_properties(${XXHASH_SOURCES} PROPERTIES COMPILE_OPTIONS -w)

afterglow_fuzz_sources(AfterglowVt VT_SOURCES)
add_executable(afterglow_vtfuzz vtfuzz.cpp ${VT_SOURCES})
target_include_directories(afterglow_vtfuzz PRIVATE $<TARGET_PROPERTY:AfterglowVt,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(afterglow_vtfuzz PRIVATE afterglow_fuzz_proto PkgConfig::PCRE2)

add_executable(afterglow_protofuzz protofuzz.cpp)
target_link_libraries(afterglow_protofuzz PRIVATE afterglow_fuzz_proto)

# The decoders as aglow builds them (tools/aglow/CMakeLists.txt): stb's code
# without Afterglow's warnings, and with -fwrapv.
afterglow_vendored_file(stb_image AFTERGLOW_FUZZ_STB_IMAGE)
get_filename_component(AFTERGLOW_FUZZ_STB_DIR "${AFTERGLOW_FUZZ_STB_IMAGE}" DIRECTORY)
set(AFTERGLOW_FUZZ_STB_SOURCE "${PROJECT_SOURCE_DIR}/tools/aglow/StbImage.cpp")
add_executable(afterglow_imagefuzz imagefuzz.cpp "${AFTERGLOW_FUZZ_STB_SOURCE}")
target_include_directories(afterglow_imagefuzz PRIVATE "${PROJECT_SOURCE_DIR}/tools/aglow")
target_include_directories(afterglow_imagefuzz SYSTEM PRIVATE "${AFTERGLOW_FUZZ_STB_DIR}")
set_source_files_properties("${AFTERGLOW_FUZZ_STB_SOURCE}" PROPERTIES COMPILE_OPTIONS "-w;-fwrapv")

foreach(target afterglow_fuzz_proto afterglow_vtfuzz afterglow_protofuzz afterglow_imagefuzz)
    set_target_properties(${target} PROPERTIES AUTOMOC OFF AUTOUIC OFF)
endforeach()
foreach(target afterglow_vtfuzz afterglow_protofuzz afterglow_imagefuzz)
    target_compile_options(${target} PRIVATE ${AFTERGLOW_FUZZ_FLAGS})
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
