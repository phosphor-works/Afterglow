# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# zlib and xxHash come from the release tarballs in extern/, so every build
# (the application, afterglow-vt and a static client) uses the same versions
# and no system package. The hashes are checked before anything is extracted.
set(AFTERGLOW_ZLIB_VERSION 1.3.2)
set(AFTERGLOW_ZLIB_SHA256 bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16)
set(AFTERGLOW_XXHASH_VERSION 0.8.4)
set(AFTERGLOW_XXHASH_SHA256 5738270935e7c3d38a79b3adf7c9692566ce7895a25f67de43ad52ab504acd32)

# Extracts extern/ARCHIVE once into the build tree and sets OUT to the
# directory it holds. The stamp is written after a whole extraction, so an
# interrupted configure extracts again instead of building half a tree.
function(afterglow_extract archive sha256 directory out)
    set(tarball "${CMAKE_CURRENT_SOURCE_DIR}/../../extern/${archive}")
    if(NOT EXISTS "${tarball}")
        message(FATAL_ERROR "The bundled ${archive} is missing from extern/. It is part of the repository.")
    endif()
    file(SHA256 "${tarball}" actual)
    if(NOT actual STREQUAL sha256)
        message(FATAL_ERROR "extern/${archive} has SHA-256 ${actual}, expected ${sha256}.")
    endif()
    set(root "${CMAKE_BINARY_DIR}/_extern")
    set(stamp "${root}/.extracted-${archive}-${sha256}")
    if(NOT EXISTS "${stamp}")
        file(REMOVE_RECURSE "${root}/${directory}")
        file(MAKE_DIRECTORY "${root}")
        file(ARCHIVE_EXTRACT INPUT "${tarball}" DESTINATION "${root}")
        file(TOUCH "${stamp}")
    endif()
    set(${out} "${root}/${directory}" PARENT_SCOPE)
endfunction()

afterglow_extract(zlib-${AFTERGLOW_ZLIB_VERSION}.tar.gz ${AFTERGLOW_ZLIB_SHA256}
    zlib-${AFTERGLOW_ZLIB_VERSION} AFTERGLOW_ZLIB_DIR)
afterglow_extract(xxHash-${AFTERGLOW_XXHASH_VERSION}.tar.gz ${AFTERGLOW_XXHASH_SHA256}
    xxHash-${AFTERGLOW_XXHASH_VERSION} AFTERGLOW_XXHASH_DIR)

# Our own targets over the upstream sources: their build scripts never run.
# Only the stream functions of zlib; the gz* file API is not used.
add_library(afterglow_zlib STATIC
    ${AFTERGLOW_ZLIB_DIR}/adler32.c
    ${AFTERGLOW_ZLIB_DIR}/compress.c
    ${AFTERGLOW_ZLIB_DIR}/crc32.c
    ${AFTERGLOW_ZLIB_DIR}/deflate.c
    ${AFTERGLOW_ZLIB_DIR}/inffast.c
    ${AFTERGLOW_ZLIB_DIR}/inflate.c
    ${AFTERGLOW_ZLIB_DIR}/inftrees.c
    ${AFTERGLOW_ZLIB_DIR}/trees.c
    ${AFTERGLOW_ZLIB_DIR}/uncompr.c
    ${AFTERGLOW_ZLIB_DIR}/zutil.c)
target_include_directories(afterglow_zlib SYSTEM PUBLIC ${AFTERGLOW_ZLIB_DIR})
add_library(afterglow_xxhash STATIC ${AFTERGLOW_XXHASH_DIR}/xxhash.c)
target_include_directories(afterglow_xxhash SYSTEM PUBLIC ${AFTERGLOW_XXHASH_DIR})
foreach(bundled afterglow_zlib afterglow_xxhash)
    # PIC: they link into the shared afterglow-vt. Hidden: a shared library
    # must not export inflate() or XXH3_*() over the copies other libraries
    # in the process (Qt, libpng) load. -w: upstream code is not ours to fix.
    set_target_properties(${bundled} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
        INTERPROCEDURAL_OPTIMIZATION OFF
        AUTOMOC OFF)
    target_compile_options(${bundled} PRIVATE -w)
endforeach()
# Both are linked into the binaries, so their notices go with them.
install(FILES ${AFTERGLOW_ZLIB_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow RENAME LICENSE.zlib)
install(FILES ${AFTERGLOW_XXHASH_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow RENAME LICENSE.xxHash)

# Static, so that a client binary can link it with no shared library beside
# it. Each binary that links it gets its own copy of the code.
add_library(AfterglowProto STATIC
    src/Clipboard.cpp
    src/Delta.cpp
    src/DragDrop.cpp
    src/Graphics.cpp
    src/Random.cpp
    src/Sha256.cpp
    src/Transfer.cpp
    src/Zlib.cpp
)
add_library(Afterglow::Proto ALIAS AfterglowProto)
target_include_directories(AfterglowProto PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(AfterglowProto PRIVATE afterglow_zlib afterglow_xxhash)
set_target_properties(AfterglowProto PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
