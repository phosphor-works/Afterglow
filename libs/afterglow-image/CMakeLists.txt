# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# The image decoders of aglow and aglow-mux (no Qt): libpng, libjpeg-turbo
# (the TurboJPEG 3 API) and libwebp, and GIF and BMP readers of its own. The
# three libraries are built from the archives that
# cmake/AfterglowDependencies.cmake pins, as static libraries, and not taken
# from the system: a static aglow then runs on a remote host whatever image
# libraries that host has, and both programs decode with the same code on
# every distribution. A security fix of a codec arrives with a new pin. Each
# codec is one target with hidden symbols (afterglow_third_party_target), and
# tools/fuzz instruments them.
set(AFTERGLOW_X86_64 "^(x86_64|AMD64|amd64)$")

# libpng from its own sources, as its CMakeLists.txt lists them: that build
# looks for a system zlib (find_package), and this one inflates with the
# bundled zlib-ng. pnglibconf.h is the prebuilt configuration that libpng
# ships for a build without its configure step. The row filters have SIMD
# code for x86-64 (SSE2) and for 64-bit Arm (NEON). The other processors get
# the C code: each switch is set, so that pngpriv.h turns none on by itself
# for sources that are not compiled here.
function(afterglow_add_libpng)
    afterglow_fetch_dependency(libpng)
    set(include "${AFTERGLOW_DEPS_BUILD_DIR}/libpng-include")
    configure_file("${libpng_SOURCE_DIR}/scripts/pnglibconf.h.prebuilt" "${include}/pnglibconf.h" COPYONLY)
    set(sources png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c pngrio.c pngrtran.c pngrutil.c pngset.c
        pngtrans.c pngwio.c pngwrite.c pngwtran.c pngwutil.c)
    set(switches PNG_ARM_NEON_OPT=0 PNG_INTEL_SSE_OPT=0 PNG_LOONGARCH_LSX_OPT=0 PNG_MIPS_MMI_OPT=0 PNG_MIPS_MSA_OPT=0
        PNG_POWERPC_VSX_OPT=0 PNG_RISCV_RVV_OPT=0)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "${AFTERGLOW_X86_64}")
        list(APPEND sources intel/intel_init.c intel/filter_sse2_intrinsics.c)
        list(TRANSFORM switches REPLACE "^PNG_INTEL_SSE_OPT=0$" "PNG_INTEL_SSE_OPT=1")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        list(APPEND sources arm/arm_init.c arm/filter_neon_intrinsics.c arm/palette_neon_intrinsics.c)
        list(TRANSFORM switches REPLACE "^PNG_ARM_NEON_OPT=0$" "PNG_ARM_NEON_OPT=2")
    endif()
    list(TRANSFORM sources PREPEND "${libpng_SOURCE_DIR}/")
    add_library(afterglow_png STATIC ${sources})
    target_compile_definitions(afterglow_png PRIVATE ${switches})
    target_include_directories(afterglow_png SYSTEM PUBLIC "${include}" "${libpng_SOURCE_DIR}")
    target_link_libraries(afterglow_png PRIVATE afterglow_zlib m)
    set(AFTERGLOW_LIBPNG_SOURCE_DIR "${libpng_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# libwebp from its own sources: the decoder, the demux library (animations),
# and the dsp and utils directories that both use. Its CMakeLists.txt is not
# added: it builds the encoder and sharpyuv too, defines HAVE_CONFIG_H for
# its whole directory and adds its options to the cache of this build.
# Without a config.h, src/dsp/cpu.h takes SSE2 on x86-64 and NEON on 64-bit
# Arm from the compiler. The SSE4.1 and AVX2 files get their flag, as
# libwebp's own builds give it, and the code picks them at run time on a
# processor that has them. No threads (WEBP_USE_THREAD): the decoders run
# with use_threads off, and libwebp then fills its function tables at the
# first decode with no lock, so one thread decodes (AfterglowImage/Decode.h
# says so: both programs decode in a child process). Hidden symbols need
# WEBP_EXTERN, or webp/types.h gives each function default visibility.
function(afterglow_add_libwebp)
    afterglow_fetch_dependency(libwebp)
    file(GLOB sources "${libwebp_SOURCE_DIR}/src/dec/*.c" "${libwebp_SOURCE_DIR}/src/demux/*.c"
         "${libwebp_SOURCE_DIR}/src/dsp/*.c" "${libwebp_SOURCE_DIR}/src/utils/*.c")
    add_library(afterglow_webp STATIC ${sources})
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "${AFTERGLOW_X86_64}")
        file(GLOB sse41 "${libwebp_SOURCE_DIR}/src/dsp/*_sse41.c")
        file(GLOB avx2 "${libwebp_SOURCE_DIR}/src/dsp/*_avx2.c")
        set_source_files_properties(${sse41} PROPERTIES COMPILE_OPTIONS -msse4.1)
        set_source_files_properties(${avx2} PROPERTIES COMPILE_OPTIONS -mavx2)
        target_compile_definitions(afterglow_webp PRIVATE WEBP_HAVE_SSE41 WEBP_HAVE_AVX2)
    endif()
    target_compile_definitions(afterglow_webp PUBLIC WEBP_EXTERN=extern)
    target_include_directories(afterglow_webp PRIVATE "${libwebp_SOURCE_DIR}")
    target_include_directories(afterglow_webp SYSTEM PUBLIC "${libwebp_SOURCE_DIR}/src")
    target_link_libraries(afterglow_webp PRIVATE m)
    set(AFTERGLOW_LIBWEBP_SOURCE_DIR "${libwebp_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# The jobs of the build of libjpeg-turbo, which runs inside one job of this
# build and does not share its job count.
set(AFTERGLOW_CODEC_BUILD_JOBS 2 CACHE STRING "Parallel jobs of the build of libjpeg-turbo, which runs as one job of this build")

# libjpeg-turbo with its own build, run apart from this one: its
# CMakeLists.txt refuses add_subdirectory(), and its SIMD code is assembly
# that its build sets up (NASM on x86-64: without NASM it builds the C code
# and decodes more slowly). Only the static TurboJPEG library is built, PIC
# and with hidden symbols as afterglow_third_party_target gives the others.
# That library also holds libspng and zlib, which libjpeg-turbo bundles for
# the image files of its TurboJPEG API: nothing here calls them, their
# symbols are hidden and have their own prefix, and LICENSE.md of
# libjpeg-turbo covers both. A fix of one of them arrives with a release of
# libjpeg-turbo.
#
# The build gets the compiler and the flags of this build, those of the build
# type too, with -O2 raised to -O3 in Release and RelWithDebInfo, as
# libjpeg-turbo raises its defaults (only at its first configure, so it is
# done here for each). A fuzz build adds its instrumentation after them, so
# that its -O1 comes last: a target of another build takes no options
# afterwards (tools/fuzz). A generator with many configurations builds the
# library once, as Release. A flag with a quote, a backslash or
# "${" does not pass: the arguments go into a CMake script as they are.
# turbojpeg.h is copied out, so that an include path does not show the other
# headers of libjpeg-turbo.
function(afterglow_add_libjpeg_turbo)
    afterglow_fetch_dependency(libjpeg-turbo)
    set(build "${AFTERGLOW_DEPS_BUILD_DIR}/libjpeg-turbo-build")
    set(include "${AFTERGLOW_DEPS_BUILD_DIR}/libjpeg-turbo-include")
    configure_file("${libjpeg-turbo_SOURCE_DIR}/src/turbojpeg.h" "${include}/turbojpeg.h" COPYONLY)

    # A build directory is for one source directory: CMake refuses another.
    # A new pin, a CPM cache in another place, or the change between CPM and
    # AFTERGLOW_DEPS_ARCHIVES gives another, and the old build goes.
    if(EXISTS "${build}/CMakeCache.txt")
        file(STRINGS "${build}/CMakeCache.txt" home REGEX "^CMAKE_HOME_DIRECTORY:INTERNAL=")
        string(REPLACE "CMAKE_HOME_DIRECTORY:INTERNAL=" "" home "${home}")
        file(REAL_PATH "${libjpeg-turbo_SOURCE_DIR}" source)
        if(NOT IS_DIRECTORY "${home}")
            file(REMOVE_RECURSE "${build}")
        else()
            file(REAL_PATH "${home}" home)
            if(NOT home STREQUAL source)
                file(REMOVE_RECURSE "${build}")
            endif()
        endif()
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "${AFTERGLOW_X86_64}")
        find_program(AFTERGLOW_NASM NAMES nasm yasm DOC "The assembler of the SIMD code of libjpeg-turbo")
        if(NOT AFTERGLOW_NASM)
            message(STATUS "libjpeg-turbo: NASM was not found, so it builds without its SIMD code, and aglow and aglow-mux decode JPEG more slowly")
        endif()
    endif()

    # A compiler of two words (CC="ccache gcc") is a list for the other build.
    set(compiler "${CMAKE_C_COMPILER}")
    if(CMAKE_C_COMPILER_ARG1)
        separate_arguments(words NATIVE_COMMAND "${CMAKE_C_COMPILER_ARG1}")
        list(APPEND compiler ${words})
    endif()
    set(arguments "-DCMAKE_C_COMPILER:STRING=${compiler}" "-DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS} -fvisibility=hidden"
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}" -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
        -DENABLE_SHARED:BOOL=OFF -DENABLE_STATIC:BOOL=ON -DWITH_TURBOJPEG:BOOL=ON -DWITH_TOOLS:BOOL=OFF
        -DWITH_TESTS:BOOL=OFF)
    set(forwarded CMAKE_TOOLCHAIN_FILE CMAKE_C_COMPILER_LAUNCHER CMAKE_C_COMPILER_TARGET CMAKE_SYSROOT)
    if(CMAKE_CROSSCOMPILING)
        # Without a toolchain file, the system of the target is in these two.
        list(APPEND forwarded CMAKE_SYSTEM_NAME CMAKE_SYSTEM_PROCESSOR)
    endif()
    foreach(variable IN LISTS forwarded)
        if(${variable})
            list(APPEND arguments "-D${variable}:STRING=${${variable}}")
        endif()
    endforeach()
    get_property(multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi)
        set(type Release)
        set(library "${build}/Release/libturbojpeg.a")
        set(config --config Release)
    else()
        set(type "${CMAKE_BUILD_TYPE}")
        if(NOT type)
            set(type Release)
        endif()
        set(library "${build}/libturbojpeg.a")
        set(config)
        list(APPEND arguments "-DCMAKE_BUILD_TYPE:STRING=${type}")
    endif()
    string(TOUPPER "${type}" type)
    set(typeFlags "${CMAKE_C_FLAGS_${type}}")
    if(type STREQUAL "RELEASE" OR type STREQUAL "RELWITHDEBINFO")
        string(REPLACE "-O2" "-O3" typeFlags "${typeFlags}")
    endif()
    if(AFTERGLOW_BUILD_FUZZERS)
        list(JOIN AFTERGLOW_FUZZ_FLAGS " " fuzz)
        string(APPEND typeFlags " ${fuzz}")
    endif()
    list(APPEND arguments "-DCMAKE_C_FLAGS_${type}:STRING=${typeFlags}")

    include(ExternalProject)
    ExternalProject_Add(afterglow_turbojpeg_build
        PREFIX "${AFTERGLOW_DEPS_BUILD_DIR}/libjpeg-turbo-steps"
        SOURCE_DIR "${libjpeg-turbo_SOURCE_DIR}"
        BINARY_DIR "${build}"
        DOWNLOAD_COMMAND ""
        CMAKE_CACHE_ARGS ${arguments}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target turbojpeg-static --parallel ${AFTERGLOW_CODEC_BUILD_JOBS} ${config}
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${library}"
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_OUTPUT_ON_FAILURE ON
        EXCLUDE_FROM_ALL ON)
    # Global: mux, tools/aglow and tools/fuzz link it from other directories.
    add_library(afterglow_turbojpeg STATIC IMPORTED GLOBAL)
    set_target_properties(afterglow_turbojpeg PROPERTIES IMPORTED_LOCATION "${library}")
    target_include_directories(afterglow_turbojpeg INTERFACE "${include}")
    add_dependencies(afterglow_turbojpeg afterglow_turbojpeg_build)
    set(AFTERGLOW_LIBJPEG_TURBO_SOURCE_DIR "${libjpeg-turbo_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

afterglow_add_libpng()
afterglow_add_libwebp()
afterglow_add_libjpeg_turbo()
afterglow_third_party_target(afterglow_png afterglow_webp)

# The codecs are linked into aglow and aglow-mux, so their notices go
# with the binaries: each entry is a file and the name that it installs
# under. LICENSE.md of libjpeg-turbo names README.ijg, which holds the
# license of the Independent JPEG Group, so that file keeps its name. That
# license asks that the documents of a program say that it is based in part
# on the work of the Independent JPEG Group: notices/IJG.txt says it where
# the binaries install, as README.md and docs/BUILD.md do in the source.
set(AFTERGLOW_IMAGE_NOTICES
    "${AFTERGLOW_LIBPNG_SOURCE_DIR}/LICENSE=LICENSE.libpng"
    "${AFTERGLOW_LIBJPEG_TURBO_SOURCE_DIR}/LICENSE.md=LICENSE.libjpeg-turbo"
    "${AFTERGLOW_LIBJPEG_TURBO_SOURCE_DIR}/README.ijg=README.ijg"
    "${CMAKE_CURRENT_SOURCE_DIR}/notices/IJG.txt=NOTICE.IJG"
    "${AFTERGLOW_LIBWEBP_SOURCE_DIR}/COPYING=LICENSE.libwebp"
    "${AFTERGLOW_LIBWEBP_SOURCE_DIR}/PATENTS=PATENTS.libwebp")
foreach(notice IN LISTS AFTERGLOW_IMAGE_NOTICES)
    string(REGEX REPLACE "=[^=]+$" "" file "${notice}")
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "The notice ${file} was not found, and it must install with the binaries.")
    endif()
endforeach()
set(AFTERGLOW_IMAGE_NOTICES "${AFTERGLOW_IMAGE_NOTICES}" PARENT_SCOPE)

# afterglow_install_image_notices(<directory>)
#
# Installs the notices of the three codecs into <directory>: the license
# directory of the application in a whole build (afterglow/CMakeLists.txt),
# and that of aglow in a build of aglow alone (tools/aglow/CMakeLists.txt).
function(afterglow_install_image_notices directory)
    foreach(notice IN LISTS AFTERGLOW_IMAGE_NOTICES)
        string(REGEX MATCH "^(.+)=([^=]+)$" matched "${notice}")
        install(FILES "${CMAKE_MATCH_1}" DESTINATION "${directory}" RENAME "${CMAKE_MATCH_2}")
    endforeach()
endfunction()

add_library(AfterglowImage STATIC
    src/Bmp.cpp
    src/Decode.cpp
    src/Gif.cpp
    src/Jpeg.cpp
    src/Png.cpp
    src/WebP.cpp
)
add_library(Afterglow::Image ALIAS AfterglowImage)
target_include_directories(AfterglowImage PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(AfterglowImage PRIVATE afterglow_png afterglow_turbojpeg afterglow_webp)
# PIC: aglow and aglow-mux are position-independent executables.
set_target_properties(AfterglowImage PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF)

# The image files of the tests, which the tests of aglow-mux and the
# seeds of the image fuzz target use too.
add_library(afterglow_image_test_files INTERFACE)
target_include_directories(afterglow_image_test_files INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tests")

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
