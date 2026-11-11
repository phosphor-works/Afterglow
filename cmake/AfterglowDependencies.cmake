# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The third-party sources that Afterglow builds into its own binaries. Each
# archive is pinned by version, URL and SHA-256. Each vendored file (a
# single-file library kept in the repository) is pinned by its upstream commit
# and the SHA-256 of the file. This file is the only place a pin lives. It
# works as cmake/PhosphorDependencies.cmake in PlasmaZones does, which pins the
# archives of the Phosphor libraries.
#
# The archives are built into the tree, not taken from the system, so that the
# terminal and a static client of libs/afterglow-proto use the same code.
#
# How an archive is obtained, in order:
#
#   1. AFTERGLOW_DEPS_ARCHIVES names a directory that holds the archives under
#      their FILE names below. Each is checked against its SHA-256 and
#      extracted into the build tree. Nothing is downloaded and CPM is never
#      loaded, so an offline packaging build takes this path.
#   2. Otherwise CPM.cmake (pinned by hash) downloads the archive once into
#      CPM_SOURCE_CACHE. When PlasmaZones' module has loaded CPM, that is its
#      cache, .cache/cpm in the PlasmaZones checkout. Else it is .cache/cpm at
#      this repository's root. The cache variable or the environment variable
#      CPM_SOURCE_CACHE overrides both. A configure works offline once the
#      cache holds the archive.
#
# Script mode, for packagers:
#
#   cmake -DAFTERGLOW_DEPS_FETCH=<dir> -P cmake/AfterglowDependencies.cmake
#
# downloads every archive into <dir> under its FILE name and checks it, so
# <dir> can be given as AFTERGLOW_DEPS_ARCHIVES. Each download also leaves an
# empty FILE.lock in <dir> (see _afterglow_download). AFTERGLOW_DEPS_ARCHIVES
# reads only the archives, so the lock files need not be copied with them.
#
# Bumping a pin: change its VERSION, URL, FILE and SHA256 together, with the
# hash taken from a download that you checked against a second source.

include_guard(GLOBAL)

# ─── Pins ────────────────────────────────────────────────────────────────────
set(AFTERGLOW_DEPS zlib-ng xxhash)

# zlib-ng (Zlib license), in its zlib-compatible build: o=z images, file
# transfer compression. The hash matches Homebrew's zlib-ng formula.
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

# ─── Vendored files ──────────────────────────────────────────────────────────
# A single-file library is kept in the repository, so that it needs no archive
# of its own. Its pin is the upstream commit and the SHA-256 of the file,
# checked at configure time by afterglow_vendored_file(). Bumping it: copy
# the file from the new commit and change COMMIT and SHA256 together.

# stb_image (MIT, Sean Barrett), v2.30: aglow icat decodes JPEG, BMP and the
# other formats with it, and afterglow-mux the PNG and iTerm2 images of its
# sessions. The file carries aglow's patches, which the top of
# tools/aglow/StbImage.cpp lists, so SHA256 is that of the patched file and
# COMMIT names the upstream base. REUSE.toml licenses the file.
set(AFTERGLOW_VENDORED_stb_image_COMMIT "d14d46f6b44456eb88beac58cba833a4135d4c89")
set(AFTERGLOW_VENDORED_stb_image_URL
    "https://raw.githubusercontent.com/nothings/stb/${AFTERGLOW_VENDORED_stb_image_COMMIT}/stb_image.h")
set(AFTERGLOW_VENDORED_stb_image_FILE "tools/aglow/stb/stb_image.h")
set(AFTERGLOW_VENDORED_stb_image_SHA256 "9ffe0bebf6a68d3fe05ba9a803d611f83c67a4c76acca0a8cf7d73e29d61248b")

# CPM.cmake itself, the same release that PlasmaZones pins.
set(AFTERGLOW_CPM_VERSION "0.43.2")
set(AFTERGLOW_CPM_SHA256 "49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa")

# The repository root, captured at include time: inside a function
# CMAKE_CURRENT_LIST_DIR names the caller.
get_filename_component(_AFTERGLOW_DEPS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Downloads URL to FILE through a .part file, renamed only once its hash
# checks, so a broken download never sits under the real name. FILE can be in
# a cache that other build directories share: a lock keeps two configures from
# writing one .part file, and the one that waited finds the file in place.
# The empty FILE.lock stays after the download. Removing it would break the
# lock: a configure that waits holds the removed file, and the next one would
# lock a new file of the same name at the same time.
function(_afterglow_download url file sha256)
    file(LOCK "${file}.lock" GUARD FUNCTION TIMEOUT 600)
    if(EXISTS "${file}")
        file(SHA256 "${file}" have)
        if(have STREQUAL sha256)
            return()
        endif()
    endif()
    file(DOWNLOAD "${url}" "${file}.part" EXPECTED_HASH SHA256=${sha256} STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE "${file}.part")
        list(GET status 1 why)
        message(FATAL_ERROR "Could not download ${url}: ${why}")
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
    return()
endif()

# ─── Configure mode ──────────────────────────────────────────────────────────
set(AFTERGLOW_DEPS_ARCHIVES "" CACHE PATH
    "Directory with the archives that cmake/AfterglowDependencies.cmake pins, for an offline build. Empty: download them with CPM")

# CPM keeps part of its state in ordinary variables of the scope that includes
# it, so it is loaded here, at the including scope, and not in a function. A
# build with PlasmaZones' module has loaded it already. An offline build never
# loads it, so it never reaches the network.
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

# afterglow_stb_image_notice(<path>)
#
# Writes the MIT notice of the vendored stb_image to <path>, for aglow and
# afterglow-mux, which link it. It is the notice in the vendored header
# itself, from the MIT alternative up to the next one. A regular expression
# cannot cut it there: CMake has no lazy match, and ".*" runs on to the end of
# the Unlicense. The file is written only when its text changes, so that a
# configure does not make the install copy it again.
function(afterglow_stb_image_notice path)
    afterglow_vendored_file(stb_image header)
    file(READ "${header}" text)
    string(FIND "${text}" "ALTERNATIVE A - MIT License\n" mit)
    string(FIND "${text}" "ALTERNATIVE B - " unlicense)
    if(mit EQUAL -1 OR unlicense LESS_EQUAL mit)
        message(FATAL_ERROR "The MIT notice was not found in ${header}")
    endif()
    math(EXPR length "${unlicense} - ${mit}")
    string(SUBSTRING "${text}" ${mit} ${length} notice)
    # Without the heading line and the rule of dashes that ends the alternative.
    string(REGEX REPLACE "^ALTERNATIVE A - MIT License\n" "" notice "${notice}")
    string(REGEX REPLACE "-+\n$" "" notice "${notice}")
    if(NOT notice MATCHES "^Copyright[^\n]*\n.*SOFTWARE\\.\n$")
        message(FATAL_ERROR "The MIT notice in ${header} does not have the expected form")
    endif()
    set(content "stb_image ${AFTERGLOW_VENDORED_stb_image_URL}, with the patches of aglow\n\n${notice}")
    if(EXISTS "${path}")
        file(READ "${path}" old)
        if(old STREQUAL content)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()

# afterglow_vendored_file(<name> <variable>)
#
# Sets <variable> to the path of the vendored file <name>, after checking it
# against its pin. A change to the file runs the configure again, so that the
# check sees it. Works in configure mode only.
function(afterglow_vendored_file name variable)
    if(NOT AFTERGLOW_VENDORED_${name}_FILE)
        message(FATAL_ERROR "afterglow_vendored_file: '${name}' is not pinned in ${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    endif()
    set(path "${_AFTERGLOW_DEPS_ROOT}/${AFTERGLOW_VENDORED_${name}_FILE}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "afterglow_vendored_file: ${path}, the vendored file of '${name}', does not exist")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" have)
    if(NOT have STREQUAL AFTERGLOW_VENDORED_${name}_SHA256)
        message(FATAL_ERROR "${path} does not match its pin (commit ${AFTERGLOW_VENDORED_${name}_COMMIT}):\n"
                            "  expected ${AFTERGLOW_VENDORED_${name}_SHA256}\n  got      ${have}")
    endif()
    set(${variable} "${path}" PARENT_SCOPE)
endfunction()

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
        # is a tree that was removed after it.
        set(root "${CMAKE_BINARY_DIR}/_afterglow_deps")
        set(dest "${root}/${name}-${version}")
        set(stamp "${root}/.${name}-${version}-${sha}")
        if(NOT EXISTS "${stamp}" OR NOT IS_DIRECTORY "${dest}")
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
