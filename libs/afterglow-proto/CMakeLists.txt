# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# zlib-ng and xxHash are built into the tree from the archives that
# cmake/AfterglowDependencies.cmake pins, not taken from the system. Every
# build (the application, afterglow-vt, afterglow-mux, the tests, the fuzz
# targets and a static client of this library) then uses the same code.
# zlib-ng has the SIMD paths that a distribution's zlib-ng has, chosen at run
# time, so the terminal decodes o=z images as fast as with the system
# library. A zlib-ng security fix arrives with a new pin. The root
# CMakeLists.txt includes cmake/AfterglowDependencies.cmake, at the top scope
# where CPM must load.

# zlib-ng with its own build, in its zlib-compatible API. Its public symbols
# get the prefix afterglow_ (zlib.h renames the calls), so they can never
# take the place of the zlib that other libraries in the process load (Qt,
# libpng), or the reverse. Only the static library is built: no tests, no
# gz* file API, no install rules. zlib-ng includes CPack, which would write
# its own package configuration as the top-level CPackConfig.cmake: it is
# written into zlib-ng's build directory instead.
function(afterglow_add_zlib_ng)
    afterglow_fetch_dependency(zlib-ng)
    set(build "${CMAKE_BINARY_DIR}/_afterglow_deps/zlib-ng-build")
    set(BUILD_TESTING OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(ZLIB_COMPAT ON)
    set(ZLIB_ALIASES OFF)
    set(ZLIB_SYMBOL_PREFIX "afterglow_")
    set(WITH_GZFILEOP OFF)
    set(WITH_NATIVE_INSTRUCTIONS OFF)
    set(SKIP_INSTALL_ALL ON)
    set(CPACK_OUTPUT_CONFIG_FILE "${build}/CPackConfig.cmake")
    set(CPACK_SOURCE_OUTPUT_CONFIG_FILE "${build}/CPackSourceConfig.cmake")
    add_subdirectory("${zlib-ng_SOURCE_DIR}" "${build}" EXCLUDE_FROM_ALL)
    set(AFTERGLOW_ZLIB_NG_SOURCE_DIR "${zlib-ng_SOURCE_DIR}" PARENT_SCOPE)
    set(AFTERGLOW_ZLIB_NG_BINARY_DIR "${build}" PARENT_SCOPE)
endfunction()
afterglow_add_zlib_ng()

afterglow_fetch_dependency(xxhash)
add_library(afterglow_xxhash STATIC ${xxhash_SOURCE_DIR}/xxhash.c)
target_include_directories(afterglow_xxhash SYSTEM PUBLIC ${xxhash_SOURCE_DIR})

# The include directories again as SYSTEM, so that -Wpedantic and the other
# warnings of Afterglow's code never fire in zlib-ng's headers.
add_library(afterglow_zlib INTERFACE)
target_link_libraries(afterglow_zlib INTERFACE zlib-ng)
target_include_directories(afterglow_zlib SYSTEM INTERFACE ${AFTERGLOW_ZLIB_NG_BINARY_DIR} ${AFTERGLOW_ZLIB_NG_SOURCE_DIR})

foreach(bundled zlib-ng afterglow_xxhash)
    # PIC: they link into the shared afterglow-vt. Hidden: a shared library
    # must not export inflate() or XXH3_*(). -w: upstream code is not ours to
    # fix. No LTO, even with CMAKE_INTERPROCEDURAL_OPTIMIZATION: the optimizer
    # would work on upstream code again when afterglow-vt links, and warnings
    # found then do not come under -w. qt_standard_project_setup() turns
    # AUTOMOC and AUTOUIC on for every target, and these have no Qt.
    set_target_properties(${bundled} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
        INTERPROCEDURAL_OPTIMIZATION OFF
        AUTOMOC OFF
        AUTOUIC OFF)
    target_compile_options(${bundled} PRIVATE -w)
endforeach()
# Both are linked into the binaries, so their notices go with them. They
# install with the other licence texts (afterglow/CMakeLists.txt).
set(AFTERGLOW_ZLIB_LICENSE "${AFTERGLOW_ZLIB_NG_SOURCE_DIR}/LICENSE.md" PARENT_SCOPE)
set(AFTERGLOW_XXHASH_LICENSE "${xxhash_SOURCE_DIR}/LICENSE" PARENT_SCOPE)

# Static and PIC: it links into the shared afterglow-vt, into
# afterglow_mux_protocol, and into each executable that uses it directly (the
# application, afterglow-mux, aglow, the tests). Each of them gets its own copy
# of the code, with hidden symbols.
add_library(AfterglowProto STATIC
    src/Clipboard.cpp
    src/Color.cpp
    src/Delta.cpp
    src/DndTree.cpp
    src/DndUri.cpp
    src/DragDrop.cpp
    src/FdDelta.cpp
    src/Graphics.cpp
    src/Ipc.cpp
    src/IpcServer.cpp
    src/Json.cpp
    src/Lock.cpp
    src/Random.cpp
    src/Remote.cpp
    src/Run.cpp
    src/Sha256.cpp
    src/Socket.cpp
    src/Transfer.cpp
    src/TransferClient.cpp
    src/TransferPath.cpp
    src/Zlib.cpp
)
add_library(Afterglow::Proto ALIAS AfterglowProto)
target_include_directories(AfterglowProto PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(AfterglowProto PRIVATE afterglow_zlib afterglow_xxhash)
set_target_properties(AfterglowProto PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
