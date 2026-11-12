# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libFuzzer targets (clang only, for C and C++): afterglow_vtfuzz for the
# terminal core and the reader of its state frames, afterglow_protofuzz for
# the protocol readers that take what a remote program sends,
# afterglow_imagefuzz for the image decoders of aglow and aglow-mux
# (libs/afterglow-image: its GIF and BMP readers, and libpng, libjpeg-turbo
# and libwebp behind it), and
# afterglow_muxfuzz for the readers of the attach stream of aglow-mux.
# afterglow_muxfuzz compiles code of mux/, which is GPL-3.0-or-later, so its
# source is too, and no other target links it. The
# sources of the libraries, xxHash and the decoders are compiled into them
# again, so that the sanitizers and the coverage instrument them. zlib-ng and
# the image codecs are instrumented in place, as the targets that their
# builds define: zlib-ng and libwebp set CPU flags for single files, and each
# codec is one target that the program links. libjpeg-turbo builds apart, and
# gets the flags when libs/afterglow-image sets that build up: CMakeLists.txt
# holds AFTERGLOW_FUZZ_FLAGS for both.
# Every binary in a fuzz build directory that links one of them then needs
# the sanitizer runtimes. Only the fuzz targets are built and supported
# there, so the configure turns the tests off, CMakeLists.txt turns aglow off
# (a static program cannot link the runtimes), and the build names its
# targets.
#
#   cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DAFTERGLOW_BUILD_FUZZERS=ON \
#       -DBUILD_TESTING=OFF
#   cmake --build build-fuzz --target afterglow_vtfuzz afterglow_protofuzz afterglow_imagefuzz afterglow_muxfuzz \
#       afterglow_imagefuzz_seeds
#   mkdir -p build-fuzz/corpus
#   ./build-fuzz/tools/fuzz/afterglow_vtfuzz -max_total_time=300 -max_len=65536 \
#       -dict=tools/fuzz/vt.dict build-fuzz/corpus/

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AFTERGLOW_BUILD_FUZZERS needs clang for C and C++ (libFuzzer): "
        "-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-32)

# Sets OUT to the sources of TARGET with absolute paths, so the fuzz targets
# compile what the library compiles.
function(afterglow_fuzz_sources target out)
    get_target_property(sources ${target} SOURCES)
    get_target_property(directory ${target} SOURCE_DIR)
    list(TRANSFORM sources PREPEND "${directory}/" REGEX "^[^/$]")
    set(${out} ${sources} PARENT_SCOPE)
endfunction()

foreach(target zlib-ng afterglow_png afterglow_webp)
    target_compile_options(${target} PRIVATE ${AFTERGLOW_FUZZ_FLAGS})
    target_link_options(${target} INTERFACE -fsanitize=address,undefined)
endforeach()
# An imported target: its code has the flags already (libs/afterglow-image).
set_property(TARGET afterglow_turbojpeg APPEND PROPERTY INTERFACE_LINK_OPTIONS -fsanitize=address,undefined)

# The protocol library with xxHash, for the three targets that read protocol
# data: the terminal core calls it too.
afterglow_fuzz_sources(AfterglowProto PROTO_SOURCES)
afterglow_fuzz_sources(afterglow_xxhash XXHASH_SOURCES)
add_library(afterglow_fuzz_proto STATIC ${PROTO_SOURCES} ${XXHASH_SOURCES})
target_include_directories(afterglow_fuzz_proto PUBLIC $<TARGET_PROPERTY:AfterglowProto,INTERFACE_INCLUDE_DIRECTORIES>)
target_include_directories(afterglow_fuzz_proto SYSTEM PRIVATE
    $<TARGET_PROPERTY:afterglow_xxhash,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(afterglow_fuzz_proto PUBLIC afterglow_zlib)
target_compile_options(afterglow_fuzz_proto PRIVATE ${AFTERGLOW_FUZZ_FLAGS})
# Upstream code is not ours to fix (as afterglow_third_party_target says).
set_source_files_properties(${XXHASH_SOURCES} PROPERTIES COMPILE_OPTIONS -w)

afterglow_fuzz_sources(AfterglowVt VT_SOURCES)
add_executable(afterglow_vtfuzz vtfuzz.cpp ${VT_SOURCES})
target_include_directories(afterglow_vtfuzz PRIVATE $<TARGET_PROPERTY:AfterglowVt,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(afterglow_vtfuzz PRIVATE afterglow_fuzz_proto PkgConfig::PCRE2)

add_executable(afterglow_protofuzz protofuzz.cpp)
target_link_libraries(afterglow_protofuzz PRIVATE afterglow_fuzz_proto)

# The codecs of the attach stream, as afterglow_mux_protocol compiles them.
add_executable(afterglow_muxfuzz muxfuzz.cpp "${PROJECT_SOURCE_DIR}/mux/protocol/MuxHost.cpp"
    "${PROJECT_SOURCE_DIR}/mux/protocol/MuxStream.cpp")
target_include_directories(afterglow_muxfuzz PRIVATE "${PROJECT_SOURCE_DIR}/mux/protocol"
    $<TARGET_PROPERTY:AfterglowVt,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(afterglow_muxfuzz PRIVATE afterglow_fuzz_proto)

# The decoders, compiled again from the sources of libs/afterglow-image.
afterglow_fuzz_sources(AfterglowImage IMAGE_SOURCES)
add_executable(afterglow_imagefuzz imagefuzz.cpp ${IMAGE_SOURCES})
target_include_directories(afterglow_imagefuzz PRIVATE $<TARGET_PROPERTY:AfterglowImage,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(afterglow_imagefuzz PRIVATE afterglow_png afterglow_turbojpeg afterglow_webp)
# The files of the tests of the decoders, as a first corpus of
# afterglow_imagefuzz. It reads no input, so it is not instrumented.
add_executable(afterglow_imagefuzz_seeds imagefuzz_seeds.cpp)
target_link_libraries(afterglow_imagefuzz_seeds PRIVATE afterglow_image_test_files)
set_target_properties(afterglow_imagefuzz_seeds PROPERTIES AUTOMOC OFF AUTOUIC OFF)

foreach(target afterglow_fuzz_proto afterglow_vtfuzz afterglow_protofuzz afterglow_imagefuzz afterglow_muxfuzz)
    set_target_properties(${target} PROPERTIES AUTOMOC OFF AUTOUIC OFF)
endforeach()
foreach(target afterglow_vtfuzz afterglow_protofuzz afterglow_imagefuzz afterglow_muxfuzz)
    target_compile_options(${target} PRIVATE ${AFTERGLOW_FUZZ_FLAGS})
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
