# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The sources that Afterglow builds into its own binaries: the phosphor-libs
# commit that it builds the Phosphor libraries from, and the third-party
# libraries. Each archive is pinned by version, URL and SHA-256. This file is
# the only place a pin lives. The phosphor-libs tree pins its own archives
# (Luau, valijson) in its cmake/PhosphorDependencies.cmake, which works the
# same way.
#
# The archives are built into the tree, not taken from the system, so that the
# terminal and the static aglow use the same code, and so that aglow needs no
# library of the host that runs it.
#
# How an archive is obtained, in order:
#
#   1. AFTERGLOW_DEPS_ARCHIVES names a directory that holds the archives under
#      their FILE names below. Each is checked against its SHA-256 and
#      extracted into the build tree. Nothing is downloaded and CPM is never
#      loaded, so an offline packaging build takes this path.
#   2. Otherwise CPM.cmake (pinned by hash) downloads the archive once into
#      CPM_SOURCE_CACHE, .cache/cpm at this repository's root. The cache
#      variable or the environment variable CPM_SOURCE_CACHE overrides it. A
#      configure works offline once the cache holds the archive. CPM's own
#      override applies: -DCPM_<name>_SOURCE=<dir> uses <dir> in place of the
#      archive.
#
# Script mode, for packagers:
#
#   cmake -DAFTERGLOW_DEPS_FETCH=<dir> -P cmake/AfterglowDependencies.cmake
#
# downloads every archive into <dir> under its FILE name and checks it, so
# <dir> can be given as AFTERGLOW_DEPS_ARCHIVES. It then downloads the archives
# that the pinned phosphor-libs tree pins into the same <dir>, which can be
# given as PHOSPHOR_DEPS_ARCHIVES too. Each download also leaves an
# empty FILE.lock in <dir> (see _afterglow_download). AFTERGLOW_DEPS_ARCHIVES
# reads only the archives, so the lock files need not be copied with them.
#
# Bumping a pin: change its VERSION, URL, FILE and SHA256 together, with the
# hash taken from a download that you checked against a second source.

include_guard(GLOBAL)

# ─── Pins ────────────────────────────────────────────────────────────────────
set(AFTERGLOW_DEPS phosphor-libs zlib-ng xxhash libpng libjpeg-turbo libwebp lcms2)

# The phosphor-libs commit (LGPL-2.1-or-later, phosphor-works/phosphor-libs)
# that the Phosphor libraries are built from (cmake/AfterglowPhosphor.cmake).
# A commit, not a release, so VERSION is the commit hash.
set(AFTERGLOW_DEP_phosphor-libs_VERSION "61efaa4ee9be8074e8528b626ebe47f760ccefb4")
set(AFTERGLOW_DEP_phosphor-libs_URL "https://github.com/phosphor-works/phosphor-libs/archive/61efaa4ee9be8074e8528b626ebe47f760ccefb4.tar.gz")
set(AFTERGLOW_DEP_phosphor-libs_FILE "phosphor-libs-61efaa4ee9be8074e8528b626ebe47f760ccefb4.tar.gz")
set(AFTERGLOW_DEP_phosphor-libs_SHA256 "2dc5541f30e31acf3762476adc5e5f0e80520bd7c6e2eae0fe3c119332b605eb")

# zlib-ng (Zlib license), in its zlib-compatible build: o=z images, file
# transfer compression, and the inflate of the bundled libpng. The hash
# matches Homebrew's zlib-ng formula.
set(AFTERGLOW_DEP_zlib-ng_VERSION "2.3.3")
set(AFTERGLOW_DEP_zlib-ng_URL "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz")
set(AFTERGLOW_DEP_zlib-ng_FILE "zlib-ng-2.3.3.tar.gz")
set(AFTERGLOW_DEP_zlib-ng_SHA256 "f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1")

# xxHash (BSD-2-Clause, Yann Collet): the XXH3 hashes of delta transfers. The
# hash matches Homebrew's xxhash formula, and the archive is tag v0.8.4.
set(AFTERGLOW_DEP_xxhash_VERSION "0.8.4")
set(AFTERGLOW_DEP_xxhash_URL "https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz")
set(AFTERGLOW_DEP_xxhash_FILE "xxHash-0.8.4.tar.gz")
set(AFTERGLOW_DEP_xxhash_SHA256 "5738270935e7c3d38a79b3adf7c9692566ce7895a25f67de43ad52ab504acd32")

# The image codecs of libs/afterglow-image, which aglow and aglow-mux link
# statically. Each is the release archive of its project, and each hash
# matches the Homebrew formula of the same version.
#
# libpng (libpng-2.0 license): PNG, through its simplified API.
set(AFTERGLOW_DEP_libpng_VERSION "1.6.59")
set(AFTERGLOW_DEP_libpng_URL "https://downloads.sourceforge.net/project/libpng/libpng16/1.6.59/libpng-1.6.59.tar.xz")
set(AFTERGLOW_DEP_libpng_FILE "libpng-1.6.59.tar.xz")
set(AFTERGLOW_DEP_libpng_SHA256 "d80dd2a38a37f803cb9b6ac7b14bd6e74ddc3b654780a8380bdf93523fdb4389")

# libjpeg-turbo (IJG and BSD-3-Clause licenses): JPEG, through the TurboJPEG 3
# API. The decoder needs 3.0.2 or later, for TJPARAM_MAXPIXELS.
set(AFTERGLOW_DEP_libjpeg-turbo_VERSION "3.2.1")
set(AFTERGLOW_DEP_libjpeg-turbo_URL "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.1/libjpeg-turbo-3.2.1.tar.gz")
set(AFTERGLOW_DEP_libjpeg-turbo_FILE "libjpeg-turbo-3.2.1.tar.gz")
set(AFTERGLOW_DEP_libjpeg-turbo_SHA256 "c27ad72a68da151177929aa969ae773a2da5ac2af9ce946079314fdac8cece7f")

# libwebp (BSD-3-Clause, with a patent grant): WebP, and its animations
# through the demux library.
set(AFTERGLOW_DEP_libwebp_VERSION "1.6.0")
set(AFTERGLOW_DEP_libwebp_URL "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz")
set(AFTERGLOW_DEP_libwebp_FILE "libwebp-1.6.0.tar.gz")
set(AFTERGLOW_DEP_libwebp_SHA256 "e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564")

# Little CMS (MIT license): the colour conversion of images with an ICC
# profile or a cICP chunk to sRGB. The hash matches the release archive on
# GitHub and the one on SourceForge.
set(AFTERGLOW_DEP_lcms2_VERSION "2.20")
set(AFTERGLOW_DEP_lcms2_URL "https://github.com/mm2/Little-CMS/releases/download/lcms2.20/lcms2-2.20.tar.gz")
set(AFTERGLOW_DEP_lcms2_FILE "lcms2-2.20.tar.gz")
set(AFTERGLOW_DEP_lcms2_SHA256 "18ff5504fa9cc8a2b1f3591180d90ead16c8a2ea78e530523b46eab70e1ca6b9")

# CPM.cmake itself, the same release that phosphor-libs pins, so that the
# phosphor-libs module uses this copy.
set(AFTERGLOW_CPM_VERSION "0.43.2")
set(AFTERGLOW_CPM_SHA256 "49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa")

# The repository root, captured at include time: inside a function
# CMAKE_CURRENT_LIST_DIR names the caller.
get_filename_component(_AFTERGLOW_DEPS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Where the archives unpack without CPM, and where the libraries built from
# them keep their builds and generated headers.
set(AFTERGLOW_DEPS_BUILD_DIR "${CMAKE_BINARY_DIR}/_afterglow_deps")

# Downloads URL to FILE through a .part file, renamed only once its hash
# checks, so a broken download never sits under the real name. FILE can be in
# a cache that other build directories share: a lock keeps two configures from
# writing one .part file, and the one that waited finds the file in place.
# The empty FILE.lock stays after the download. Removing it would break the
# lock: a configure that waits holds the removed file, and the next one would
# lock a new file of the same name at the same time.
#
# The hash is checked here, not by file(DOWNLOAD): with EXPECTED_HASH a failed
# transfer or a wrong hash stops the configure inside file(DOWNLOAD), before
# the .part file can be removed. A server that sends nothing for a minute
# ends the download, so that a configure does not wait without end while it
# holds the lock.
function(_afterglow_download url file sha256)
    file(LOCK "${file}.lock" GUARD FUNCTION TIMEOUT 600)
    if(EXISTS "${file}")
        file(SHA256 "${file}" have)
        if(have STREQUAL sha256)
            return()
        endif()
    endif()
    file(DOWNLOAD "${url}" "${file}.part" INACTIVITY_TIMEOUT 60 STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE "${file}.part")
        list(GET status 1 why)
        message(FATAL_ERROR "Could not download ${url}: ${why}")
    endif()
    file(SHA256 "${file}.part" have)
    if(NOT have STREQUAL sha256)
        file(REMOVE "${file}.part")
        message(FATAL_ERROR "${url} does not match its pin:\n  expected ${sha256}\n  got      ${have}")
    endif()
    file(RENAME "${file}.part" "${file}")
endfunction()

# ─── Script mode: download the archives ──────────────────────────────────────
if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT AFTERGLOW_DEPS_FETCH)
        message(FATAL_ERROR "usage: cmake -DAFTERGLOW_DEPS_FETCH=<dir> -P ${CMAKE_CURRENT_LIST_FILE}")
    endif()
    get_filename_component(dir "${AFTERGLOW_DEPS_FETCH}" ABSOLUTE)
    file(MAKE_DIRECTORY "${dir}")
    foreach(name IN LISTS AFTERGLOW_DEPS)
        set(file "${dir}/${AFTERGLOW_DEP_${name}_FILE}")
        set(have "")
        if(EXISTS "${file}")
            file(SHA256 "${file}" have)
        endif()
        if(NOT have STREQUAL AFTERGLOW_DEP_${name}_SHA256)
            message(STATUS "Downloading ${AFTERGLOW_DEP_${name}_URL}")
            _afterglow_download("${AFTERGLOW_DEP_${name}_URL}" "${file}" "${AFTERGLOW_DEP_${name}_SHA256}")
        endif()
        message(STATUS "${AFTERGLOW_DEP_${name}_FILE}: ok")
    endforeach()
    # The archives that the pinned phosphor-libs tree pins, read from that tree,
    # so that a phosphor-libs bump never leaves this list behind.
    set(libs "${dir}/.phosphor-libs-${AFTERGLOW_DEP_phosphor-libs_VERSION}")
    file(REMOVE_RECURSE "${libs}")
    file(ARCHIVE_EXTRACT INPUT "${dir}/${AFTERGLOW_DEP_phosphor-libs_FILE}" DESTINATION "${libs}")
    file(GLOB top LIST_DIRECTORIES true "${libs}/*")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DPHOSPHOR_DEPS_FETCH=${dir}"
                            -P "${top}/cmake/PhosphorDependencies.cmake"
                    RESULT_VARIABLE rc)
    file(REMOVE_RECURSE "${libs}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Could not download the archives that phosphor-libs "
                            "${AFTERGLOW_DEP_phosphor-libs_VERSION} pins")
    endif()
    return()
endif()

# ─── Configure mode ──────────────────────────────────────────────────────────
set(AFTERGLOW_DEPS_ARCHIVES "" CACHE PATH
    "Directory with the archives that cmake/AfterglowDependencies.cmake pins, for an offline build. Empty: download them with CPM")

# CPM keeps part of its state in ordinary variables of the scope that includes
# it, so it is loaded here, at the including scope, and not in a function. The
# phosphor-libs module, included after this one, uses it again. An offline
# build never loads it, so it never reaches the network.
if(NOT AFTERGLOW_DEPS_ARCHIVES AND NOT COMMAND CPMAddPackage)
    if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
        set(CPM_SOURCE_CACHE "${_AFTERGLOW_DEPS_ROOT}/.cache/cpm" CACHE PATH
            "Where CPM.cmake keeps downloaded packages, shared by every build directory")
    endif()
    if(DEFINED CPM_SOURCE_CACHE)
        set(_afterglow_cpm_file "${CPM_SOURCE_CACHE}/cpm/CPM_${AFTERGLOW_CPM_VERSION}.cmake")
    else()
        set(_afterglow_cpm_file "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${AFTERGLOW_CPM_VERSION}.cmake")
    endif()
    # Checked on every configure, so a broken copy in the cache is replaced.
    set(_afterglow_cpm_have "")
    if(EXISTS "${_afterglow_cpm_file}")
        file(SHA256 "${_afterglow_cpm_file}" _afterglow_cpm_have)
    endif()
    if(NOT _afterglow_cpm_have STREQUAL AFTERGLOW_CPM_SHA256)
        get_filename_component(_afterglow_cpm_dir "${_afterglow_cpm_file}" DIRECTORY)
        file(MAKE_DIRECTORY "${_afterglow_cpm_dir}")
        _afterglow_download(
            "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${AFTERGLOW_CPM_VERSION}/CPM.cmake"
            "${_afterglow_cpm_file}" "${AFTERGLOW_CPM_SHA256}")
    endif()
    include("${_afterglow_cpm_file}")
endif()

# afterglow_fetch_dependency(<name>)
#
# Makes the pinned archive <name> (one of AFTERGLOW_DEPS) available and sets
# <name>_SOURCE_DIR in the caller's scope to its unpacked tree, without its
# single top-level directory. What to build from it is the caller's business.
function(afterglow_fetch_dependency name)
    if(NOT name IN_LIST AFTERGLOW_DEPS)
        message(FATAL_ERROR "afterglow_fetch_dependency: '${name}' is not pinned in ${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    endif()
    set(version "${AFTERGLOW_DEP_${name}_VERSION}")
    set(sha "${AFTERGLOW_DEP_${name}_SHA256}")

    if(AFTERGLOW_DEPS_ARCHIVES)
        set(archive "${AFTERGLOW_DEPS_ARCHIVES}/${AFTERGLOW_DEP_${name}_FILE}")
        if(NOT EXISTS "${archive}")
            message(FATAL_ERROR
                "AFTERGLOW_DEPS_ARCHIVES is ${AFTERGLOW_DEPS_ARCHIVES}, which has no ${AFTERGLOW_DEP_${name}_FILE}. "
                "Fetch it from ${AFTERGLOW_DEP_${name}_URL}, or fill the directory with "
                "cmake -DAFTERGLOW_DEPS_FETCH=<dir> -P cmake/AfterglowDependencies.cmake")
        endif()
        file(SHA256 "${archive}" have)
        if(NOT have STREQUAL sha)
            message(FATAL_ERROR "${archive} does not match its pin:\n  expected ${sha}\n  got      ${have}")
        endif()
        # The hash is part of the stamp, so a re-pinned archive under the same
        # version extracts again. The stamp is written after a whole
        # extraction, so one cut short is redone on the next configure, and so
        # is a tree that was removed after it. The stamps of the other hashes
        # of this version go before an extraction: the tree is no longer
        # theirs, and a return to an older pin must extract it again.
        set(dest "${AFTERGLOW_DEPS_BUILD_DIR}/${name}-${version}")
        set(stamp "${AFTERGLOW_DEPS_BUILD_DIR}/.${name}-${version}-${sha}")
        if(NOT EXISTS "${stamp}" OR NOT IS_DIRECTORY "${dest}")
            file(GLOB stale LIST_DIRECTORIES false "${AFTERGLOW_DEPS_BUILD_DIR}/.${name}-${version}-*")
            if(stale)
                file(REMOVE ${stale})
            endif()
            file(REMOVE_RECURSE "${dest}" "${dest}.tmp")
            file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dest}.tmp")
            file(GLOB top LIST_DIRECTORIES true "${dest}.tmp/*")
            list(LENGTH top count)
            if(count EQUAL 1 AND IS_DIRECTORY "${top}")
                file(RENAME "${top}" "${dest}")
                file(REMOVE_RECURSE "${dest}.tmp")
            else()
                file(RENAME "${dest}.tmp" "${dest}")
            endif()
            file(TOUCH "${stamp}")
        endif()
        set(${name}_SOURCE_DIR "${dest}" PARENT_SCOPE)
        return()
    endif()

    # The pinned archive, never an installed package: find_package would set
    # no source tree, and the system copy is not the pinned code. These are
    # function variables, so the caller's CPM settings stay as they are.
    set(CPM_USE_LOCAL_PACKAGES OFF)
    set(CPM_LOCAL_PACKAGES_ONLY OFF)
    CPMAddPackage(NAME ${name} VERSION ${version}
                  URL "${AFTERGLOW_DEP_${name}_URL}"
                  URL_HASH SHA256=${sha}
                  DOWNLOAD_ONLY YES)
    if(NOT IS_DIRECTORY "${${name}_SOURCE_DIR}")
        message(FATAL_ERROR "CPM gave no source tree for ${name} ${version} (${name}_SOURCE_DIR is "
                            "'${${name}_SOURCE_DIR}'). Check CPM_${name}_SOURCE and CPM_SOURCE_CACHE.")
    endif()
    set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# afterglow_third_party_target(<target>...)
#
# The properties of a library that is built from a pinned archive. PIC: it can
# link into the shared afterglow-vt. Hidden: an executable or a shared library
# that holds it must not export inflate(), XXH3_*() or png_*(), which the
# system's copy of the library in the same process (Qt loads zlib, libpng and
# libjpeg) could then take, or the reverse. -w: upstream code is not ours to
# fix. No LTO, even with CMAKE_INTERPROCEDURAL_OPTIMIZATION: the optimizer
# would work on upstream code again at each link, and warnings found then do
# not come under -w. qt_standard_project_setup() turns AUTOMOC and AUTOUIC on
# for every target, and these have no Qt.
function(afterglow_third_party_target)
    foreach(target IN LISTS ARGN)
        set_target_properties(${target} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            C_VISIBILITY_PRESET hidden
            INTERPROCEDURAL_OPTIMIZATION OFF
            AUTOMOC OFF
            AUTOUIC OFF)
        target_compile_options(${target} PRIVATE -w)
    endforeach()
endfunction()

# afterglow_install_bundled_notices(<directory>)
#
# Installs into <directory> the notices of the third-party code and data that
# the binaries hold: zlib-ng and xxHash (libs/afterglow-proto), the image
# codecs and Little CMS (AFTERGLOW_IMAGE_NOTICES of libs/afterglow-image), and
# the license of the Unicode Character Database, from which libs/afterglow-vt
# has its generated tables (UnicodeTables.inc). Its callers are the license
# directory of the application in a whole build (afterglow/CMakeLists.txt),
# and those of aglow (tools/aglow/CMakeLists.txt) and aglow-mux
# (mux/CMakeLists.txt) in a build of the two alone. aglow holds no Unicode
# table, and gets the same set: the programs of Afterglow carry one set of
# notices. It reads the variables that libs/afterglow-proto and
# libs/afterglow-image set, so it runs in a directory added after them.
function(afterglow_install_bundled_notices directory)
    # A notice that is missing is an error of the configure, not of the install.
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/LICENSES/Unicode-3.0.txt")
        message(FATAL_ERROR "The notice LICENSES/Unicode-3.0.txt was not found, and it must install with the binaries.")
    endif()
    install(FILES "${AFTERGLOW_ZLIB_LICENSE}" DESTINATION "${directory}" RENAME LICENSE.zlib-ng)
    install(FILES "${AFTERGLOW_XXHASH_LICENSE}" DESTINATION "${directory}" RENAME LICENSE.xxHash)
    foreach(notice IN LISTS AFTERGLOW_IMAGE_NOTICES)
        string(REGEX MATCH "^(.+)=([^=]+)$" matched "${notice}")
        install(FILES "${CMAKE_MATCH_1}" DESTINATION "${directory}" RENAME "${CMAKE_MATCH_2}")
    endforeach()
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSES/Unicode-3.0.txt" DESTINATION "${directory}" RENAME LICENSE.Unicode)
endfunction()
