# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# The image decoders of aglow and afterglow-mux (no Qt): libpng, libjpeg-turbo
# (the TurboJPEG 3 API) and libwebp, and GIF and BMP readers of its own. The
# three libraries are built from the archives that
# cmake/AfterglowDependencies.cmake pins, as static libraries, and not taken
# from the system: a static aglow then runs on a remote host whatever image
# libraries that host has, and both programs decode with the same code on
# every distribution. A security fix of a codec arrives with a new pin. Each
# codec is one target with hidden symbols (afterglow_third_party_target), and
# tools/fuzz instruments them.
set(AFTERGLOW_IMAGE_DEPS "${CMAKE_BINARY_DIR}/_afterglow_deps")

# libpng from its own sources, as its CMakeLists.txt lists them: that build
# looks for a system zlib (find_package), and this one inflates with the
# bundled zlib-ng. pnglibconf.h is the prebuilt configuration that libpng
# ships for a build without its configure step. The row filters have SIMD
# code for x86-64 (SSE2) and for 64-bit Arm (NEON). The other processors get
# the C code: each switch is set, so that pngpriv.h turns none on by itself
# for sources that are not compiled here.
function(afterglow_add_libpng)
    afterglow_fetch_dependency(libpng)
    set(include "${AFTERGLOW_IMAGE_DEPS}/libpng-include")
    configure_file("${libpng_SOURCE_DIR}/scripts/pnglibconf.h.prebuilt" "${include}/pnglibconf.h" COPYONLY)
    set(sources png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c pngrio.c pngrtran.c pngrutil.c pngset.c
        pngtrans.c pngwio.c pngwrite.c pngwtran.c pngwutil.c)
    set(switches PNG_ARM_NEON_OPT=0 PNG_INTEL_SSE_OPT=0 PNG_LOONGARCH_LSX_OPT=0 PNG_MIPS_MMI_OPT=0 PNG_MIPS_MSA_OPT=0
        PNG_POWERPC_VSX_OPT=0 PNG_RISCV_RVV_OPT=0)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
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
# added: it sets CMAKE_BUILD_TYPE in the cache of the whole build. Without a
# config.h, src/dsp/cpu.h takes SSE2 on x86-64 and NEON on 64-bit Arm from the
# compiler. The SSE4.1 and AVX2 files get their flag, as libwebp's own builds
# give it, and the code picks them at run time on a processor that has them.
# No threads: the decoders run with use_threads off. Hidden symbols need
# WEBP_EXTERN, or webp/types.h gives each function default visibility.
function(afterglow_add_libwebp)
    afterglow_fetch_dependency(libwebp)
    file(GLOB sources "${libwebp_SOURCE_DIR}/src/dec/*.c" "${libwebp_SOURCE_DIR}/src/demux/*.c"
         "${libwebp_SOURCE_DIR}/src/dsp/*.c" "${libwebp_SOURCE_DIR}/src/utils/*.c")
    add_library(afterglow_webp STATIC ${sources})
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
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

# libjpeg-turbo with its own build, run apart from this one: its
# CMakeLists.txt refuses add_subdirectory(), and its SIMD code is assembly
# that its build sets up (NASM on x86-64: without NASM it builds the C code
# and decodes more slowly). Only the static TurboJPEG library is built, with
# the compiler and the flags of this build, PIC and with hidden symbols as
# afterglow_third_party_target gives the others. A fuzz build adds its
# instrumentation here, because a target of another build takes no options
# afterwards (tools/fuzz). A generator with many configurations builds it
# once, as Release. turbojpeg.h is copied out, so that an include path does
# not show the other headers of libjpeg-turbo.
function(afterglow_add_libjpeg_turbo)
    afterglow_fetch_dependency(libjpeg-turbo)
    set(build "${AFTERGLOW_IMAGE_DEPS}/libjpeg-turbo-build")
    set(include "${AFTERGLOW_IMAGE_DEPS}/libjpeg-turbo-include")
    configure_file("${libjpeg-turbo_SOURCE_DIR}/src/turbojpeg.h" "${include}/turbojpeg.h" COPYONLY)

    set(flags "${CMAKE_C_FLAGS} -fvisibility=hidden")
    if(AFTERGLOW_BUILD_FUZZERS)
        list(JOIN AFTERGLOW_FUZZ_FLAGS " " fuzz)
        string(APPEND flags " ${fuzz}")
    endif()
    set(arguments "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}" "-DCMAKE_C_FLAGS:STRING=${flags}"
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}" -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
        -DENABLE_SHARED:BOOL=OFF -DENABLE_STATIC:BOOL=ON -DWITH_TURBOJPEG:BOOL=ON -DWITH_TOOLS:BOOL=OFF
        -DWITH_TESTS:BOOL=OFF -DWITH_JAVA:BOOL=OFF)
    foreach(variable CMAKE_TOOLCHAIN_FILE CMAKE_C_COMPILER_LAUNCHER)
        if(${variable})
            list(APPEND arguments "-D${variable}:STRING=${${variable}}")
        endif()
    endforeach()
    get_property(multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi)
        set(library "${build}/Release/libturbojpeg.a")
        set(config --config Release)
    else()
        set(library "${build}/libturbojpeg.a")
        set(config)
        set(type "${CMAKE_BUILD_TYPE}")
        if(NOT type)
            set(type Release)
        endif()
        list(APPEND arguments "-DCMAKE_BUILD_TYPE:STRING=${type}")
        string(TOUPPER "${type}" type)
        if(DEFINED CMAKE_C_FLAGS_${type})
            list(APPEND arguments "-DCMAKE_C_FLAGS_${type}:STRING=${CMAKE_C_FLAGS_${type}}")
        endif()
    endif()

    include(ExternalProject)
    ExternalProject_Add(afterglow_turbojpeg_build
        PREFIX "${AFTERGLOW_IMAGE_DEPS}/libjpeg-turbo-steps"
        SOURCE_DIR "${libjpeg-turbo_SOURCE_DIR}"
        BINARY_DIR "${build}"
        DOWNLOAD_COMMAND ""
        CMAKE_CACHE_ARGS ${arguments}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target turbojpeg-static ${config}
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${library}"
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_OUTPUT_ON_FAILURE ON
        EXCLUDE_FROM_ALL ON)
    # Global: tools/fuzz links it from another directory.
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

# The codecs are linked into aglow and afterglow-mux, so their notices go
# with the binaries: each entry is a file and the name that it installs
# under. LICENSE.md of libjpeg-turbo names README.ijg, which holds the
# licence of the Independent JPEG Group, so that file keeps its name.
set(AFTERGLOW_IMAGE_NOTICES
    "${AFTERGLOW_LIBPNG_SOURCE_DIR}/LICENSE=LICENSE.libpng"
    "${AFTERGLOW_LIBJPEG_TURBO_SOURCE_DIR}/LICENSE.md=LICENSE.libjpeg-turbo"
    "${AFTERGLOW_LIBJPEG_TURBO_SOURCE_DIR}/README.ijg=README.ijg"
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
# Installs the notices of the three codecs into <directory>: the licence
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
set_target_properties(AfterglowImage PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
