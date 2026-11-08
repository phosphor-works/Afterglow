# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# cmake -DSOURCE=<dir> -DBINARY=<dir> -DGENERATOR=<name> [-DMAKE_PROGRAM=<path>]
#       [-DBUILD_TYPE=<type>] [-DC_COMPILER=<path>] [-DCXX_COMPILER=<path>]
#       [-DC_FLAGS=<flags>] [-DCXX_FLAGS=<flags>] [-DLINKER_FLAGS=<flags>]
#       [-DCONFIGS=<TYPE>,...] [-DC_FLAGS_<TYPE>=<flags>] [-DCXX_FLAGS_<TYPE>=<flags>]
#       [-DLINKER_FLAGS_<TYPE>=<flags>]
#       [-DCPM_CACHE=<dir>] [-DDEPS_ARCHIVES=<dir>] [-DJOBS=<n>] -P AglowOnlyCheck.cmake
#
# Builds aglow alone (AFTERGLOW_AGLOW_ONLY) from SOURCE in BINARY, as a remote
# host or a distribution package does, and installs it into BINARY/prefix.
# Fails unless:
#
#   - the configuration succeeds with find_package(Qt6) disabled and with
#     AFTERGLOW_PHOSPHOR_LIBS_DIR naming a directory that does not exist, and
#     CMake reports both variables as unused: nothing looked for Qt or read
#     the Phosphor checkout;
#   - the build succeeds;
#   - the prefix holds bin/aglow, its completions and its licence notices,
#     and nothing else;
#   - the installed aglow runs.
#
# The tools, the build type and the flags are those of the build that runs the
# test, so that aglow alone links as its aglow does: dynamically when the flags
# hold -fsanitize. CONFIGS names the build types in upper case, and the flags
# of each type come as they are, also empty, as that build has them. CPM_CACHE and DEPS_ARCHIVES are those of that build too, so
# that the pinned archives come from where that build took them.

foreach(required SOURCE BINARY GENERATOR)
    if(NOT ${required})
        message(FATAL_ERROR "usage: cmake -DSOURCE=<dir> -DBINARY=<dir> -DGENERATOR=<name> -P ${CMAKE_CURRENT_LIST_FILE}")
    endif()
endforeach()
if(NOT JOBS)
    set(JOBS 2)
endif()

# Runs a command and stops with its output when it fails. Its output is in
# `output` afterwards.
function(aglow_only_run what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "The ${what} of aglow alone failed (${status}):\n${out}")
    endif()
    set(output "${out}" PARENT_SCOPE)
endfunction()

set(build "${BINARY}/build")
set(prefix "${BINARY}/prefix")
file(REMOVE_RECURSE "${BINARY}")

set(args -S "${SOURCE}" -B "${build}" -G "${GENERATOR}"
    -DAFTERGLOW_AGLOW_ONLY=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON
    "-DAFTERGLOW_PHOSPHOR_LIBS_DIR=${BINARY}/no-phosphor-libs"
    -DBUILD_TESTING=OFF)
foreach(pair "CMAKE_MAKE_PROGRAM;MAKE_PROGRAM" "CMAKE_BUILD_TYPE;BUILD_TYPE" "CMAKE_C_COMPILER;C_COMPILER"
             "CMAKE_CXX_COMPILER;CXX_COMPILER" "CMAKE_C_FLAGS;C_FLAGS" "CMAKE_CXX_FLAGS;CXX_FLAGS"
             "CMAKE_EXE_LINKER_FLAGS;LINKER_FLAGS" "CPM_SOURCE_CACHE;CPM_CACHE" "AFTERGLOW_DEPS_ARCHIVES;DEPS_ARCHIVES")
    list(GET pair 0 name)
    list(GET pair 1 given)
    if(${given})
        list(APPEND args "-D${name}=${${given}}")
    endif()
endforeach()

string(REPLACE "," ";" configs "${CONFIGS}")
foreach(config IN LISTS configs)
    foreach(pair "CMAKE_C_FLAGS_${config};C_FLAGS_${config}" "CMAKE_CXX_FLAGS_${config};CXX_FLAGS_${config}"
                 "CMAKE_EXE_LINKER_FLAGS_${config};LINKER_FLAGS_${config}")
        list(GET pair 0 name)
        list(GET pair 1 given)
        if(DEFINED ${given})
            list(APPEND args "-D${name}=${${given}}")
        endif()
    endforeach()
endforeach()

aglow_only_run(configuration ${CMAKE_COMMAND} ${args})
# CMake lists the variables of the command line that nothing read.
string(FIND "${output}" "Manually-specified variables were not used by the project:" unused)
if(unused EQUAL -1)
    message(FATAL_ERROR "The configuration read CMAKE_DISABLE_FIND_PACKAGE_Qt6 and AFTERGLOW_PHOSPHOR_LIBS_DIR:\n${output}")
endif()
string(SUBSTRING "${output}" ${unused} -1 unused)
foreach(name CMAKE_DISABLE_FIND_PACKAGE_Qt6 AFTERGLOW_PHOSPHOR_LIBS_DIR)
    if(NOT unused MATCHES "\n +${name}\n")
        message(FATAL_ERROR "The configuration read ${name}: it looked for Qt or for the Phosphor libraries.\n${output}")
    endif()
endforeach()

aglow_only_run(build ${CMAKE_COMMAND} --build "${build}" --parallel ${JOBS})
aglow_only_run(installation ${CMAKE_COMMAND} --install "${build}" --prefix "${prefix}")

# A static aglow also carries the notices of glibc and libstdc++, and the
# texts of the licences that they name.
file(STRINGS "${build}/CMakeCache.txt" static REGEX "^AGLOW_STATIC_LINK_WORKS:INTERNAL=1$")
set(expected bin/aglow share/bash-completion/completions/aglow share/fish/vendor_completions.d/aglow.fish
    share/zsh/site-functions/_aglow share/licenses/aglow/LGPL-2.1-or-later.txt
    share/licenses/aglow/LICENSE.stb_image share/licenses/aglow/LICENSE.xxHash share/licenses/aglow/LICENSE.zlib-ng)
if(static)
    list(APPEND expected share/licenses/aglow/GPL-3.0-or-later.txt share/licenses/aglow/LICENSE.glibc
         share/licenses/aglow/LICENSE.libstdc++)
endif()
file(GLOB_RECURSE installed LIST_DIRECTORIES false RELATIVE "${prefix}" "${prefix}/*")
list(SORT expected)
list(SORT installed)
if(NOT installed STREQUAL expected)
    set(extra ${installed})
    list(REMOVE_ITEM extra ${expected})
    set(missing ${expected})
    if(installed)
        list(REMOVE_ITEM missing ${installed})
    endif()
    message(FATAL_ERROR "The installation of aglow alone is not bin/aglow, its completions and its licence notices.\n"
                        "  not expected: ${extra}\n  missing: ${missing}")
endif()

aglow_only_run("installed aglow" "${prefix}/bin/aglow" --version)
if(NOT output MATCHES "^aglow [0-9.]+ \\(protocol [0-9]+\\)\n$")
    message(FATAL_ERROR "The installed aglow printed an unexpected version:\n${output}")
endif()
list(LENGTH installed count)
message(STATUS "aglow alone: configured without Qt and Phosphor, built, and installed as ${count} files")
