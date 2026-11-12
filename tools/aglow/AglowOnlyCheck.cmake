# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# cmake -DSOURCE=<dir> -DBINARY=<dir> -DGENERATOR=<name> -DPHOSPHOR_LIBS=<dir> [-DMAKE_PROGRAM=<path>]
#       [-DBUILD_TYPE=<type>] [-DC_COMPILER=<path>] [-DCXX_COMPILER=<path>]
#       [-DC_FLAGS=<flags>] [-DCXX_FLAGS=<flags>] [-DLINKER_FLAGS=<flags>]
#       [-DCONFIGS=<TYPE>,...] [-DC_FLAGS_<TYPE>=<flags>] [-DCXX_FLAGS_<TYPE>=<flags>]
#       [-DLINKER_FLAGS_<TYPE>=<flags>]
#       [-DCPM_CACHE=<dir>] [-DDEPS_ARCHIVES=<dir>] [-DCONFIG=<type>] [-DJOBS=<n>] -P AglowOnlyCheck.cmake
#
# Builds aglow and aglow-mux alone (AFTERGLOW_AGLOW_ONLY) from SOURCE in
# BINARY, as a remote host or a distribution package does, and installs them
# into BINARY/prefix. Fails unless:
#
#   - without aglow-mux (AFTERGLOW_BUILD_MUX off), the configuration succeeds
#     with find_package(Qt6) disabled and with AFTERGLOW_PHOSPHOR_LIBS_DIR
#     naming a directory that does not exist, and CMake reports both
#     variables as unused: nothing looked for Qt or read the Phosphor
#     checkout;
#   - with aglow-mux and the Phosphor checkout PHOSPHOR_LIBS, the
#     configuration succeeds with find_package(Qt6) disabled, and CMake
#     reports that variable as unused;
#   - that build succeeds;
#   - the prefix holds bin/aglow and bin/aglow-mux, the private libraries of
#     aglow-mux, the completions of aglow and the license notices of both,
#     and nothing else;
#   - the installed aglow and aglow-mux run.
#
# The tools, the build type and the flags are those of the build that runs the
# test, so that aglow alone links as its aglow does: dynamically when the flags
# hold -fsanitize. CONFIG is the build type that a generator with many
# configurations builds and installs. CONFIGS names the build types in upper
# case, and the flags of each type come as they are, also empty, as that
# build has them.
# CPM_CACHE and DEPS_ARCHIVES are those of that build too, so that the pinned
# archives come from where that build took them.

foreach(required SOURCE BINARY GENERATOR PHOSPHOR_LIBS)
    if(NOT ${required})
        message(FATAL_ERROR "usage: cmake -DSOURCE=<dir> -DBINARY=<dir> -DGENERATOR=<name> -DPHOSPHOR_LIBS=<dir> "
                            "-P ${CMAKE_CURRENT_LIST_FILE}")
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
        message(FATAL_ERROR "The ${what} of aglow and aglow-mux alone failed (${status}):\n${out}")
    endif()
    set(output "${out}" PARENT_SCOPE)
endfunction()

set(build "${BINARY}/build")
set(prefix "${BINARY}/prefix")
file(REMOVE_RECURSE "${BINARY}")

set(args -G "${GENERATOR}"
    -DAFTERGLOW_AGLOW_ONLY=ON
    "-DAFTERGLOW_CODEC_BUILD_JOBS=${JOBS}"
    -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON
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

# Fails unless CMake reports each variable of NAMES as unused in the output
# of a configuration: CMake lists the variables of the command line that
# nothing read.
function(aglow_only_unused output)
    string(FIND "${output}" "Manually-specified variables were not used by the project:" unused)
    if(unused EQUAL -1)
        message(FATAL_ERROR "The configuration read ${ARGN}:\n${output}")
    endif()
    string(SUBSTRING "${output}" ${unused} -1 unused)
    foreach(name IN LISTS ARGN)
        if(NOT unused MATCHES "\n +${name}\n")
            message(FATAL_ERROR "The configuration read ${name}: it looked for Qt or for the Phosphor libraries.\n${output}")
        endif()
    endforeach()
endfunction()

# aglow without aglow-mux reads no Phosphor checkout. Only configured: the
# build with aglow-mux compiles the same aglow.
aglow_only_run("configuration without aglow-mux" ${CMAKE_COMMAND} -S "${SOURCE}" -B "${BINARY}/no-mux" ${args}
               -DAFTERGLOW_BUILD_MUX=OFF "-DAFTERGLOW_PHOSPHOR_LIBS_DIR=${BINARY}/no-phosphor-libs")
aglow_only_unused("${output}" CMAKE_DISABLE_FIND_PACKAGE_Qt6 AFTERGLOW_PHOSPHOR_LIBS_DIR)
file(REMOVE_RECURSE "${BINARY}/no-mux")

aglow_only_run(configuration ${CMAKE_COMMAND} -S "${SOURCE}" -B "${build}" ${args}
               -DAFTERGLOW_BUILD_MUX=ON "-DAFTERGLOW_PHOSPHOR_LIBS_DIR=${PHOSPHOR_LIBS}")
aglow_only_unused("${output}" CMAKE_DISABLE_FIND_PACKAGE_Qt6)

set(config)
if(CONFIG)
    set(config --config "${CONFIG}")
endif()
aglow_only_run(build ${CMAKE_COMMAND} --build "${build}" --parallel ${JOBS} ${config})
aglow_only_run(installation ${CMAKE_COMMAND} --install "${build}" --prefix "${prefix}" ${config})

# A static aglow also carries the notices of glibc and libstdc++, and the
# texts of the licenses that they name, when it holds those libraries.
file(STRINGS "${build}/CMakeCache.txt" static REGEX "^AGLOW_STATIC_LINK_WORKS:INTERNAL=1$")
file(STRINGS "${build}/CMakeCache.txt" glibc REGEX "^AGLOW_GLIBC:INTERNAL=1$")
file(STRINGS "${build}/CMakeCache.txt" libstdcxx REGEX "^AGLOW_LIBSTDCXX:INTERNAL=1$")
file(STRINGS "${build}/CMakeCache.txt" libdir REGEX "^CMAKE_INSTALL_LIBDIR:PATH=")
string(REGEX REPLACE "^[^=]*=" "" libdir "${libdir}")
set(expected bin/aglow share/bash-completion/completions/aglow share/fish/vendor_completions.d/aglow.fish
    share/zsh/site-functions/_aglow share/licenses/aglow/LGPL-2.1-or-later.txt
    share/licenses/aglow/LICENSE.xxHash share/licenses/aglow/LICENSE.zlib-ng
    share/licenses/aglow/LICENSE.libpng share/licenses/aglow/LICENSE.libjpeg-turbo share/licenses/aglow/README.ijg
    share/licenses/aglow/NOTICE.IJG share/licenses/aglow/LICENSE.libwebp share/licenses/aglow/PATENTS.libwebp
    bin/aglow-mux share/licenses/aglow-mux/GPL-3.0-or-later.txt share/licenses/aglow-mux/LGPL-2.1-or-later.txt
    share/licenses/aglow-mux/LICENSE.xxHash share/licenses/aglow-mux/LICENSE.zlib-ng
    share/licenses/aglow-mux/LICENSE.libpng share/licenses/aglow-mux/LICENSE.libjpeg-turbo
    share/licenses/aglow-mux/README.ijg share/licenses/aglow-mux/NOTICE.IJG share/licenses/aglow-mux/LICENSE.libwebp
    share/licenses/aglow-mux/PATENTS.libwebp)
if(static AND glibc)
    list(APPEND expected share/licenses/aglow/LICENSE.glibc)
endif()
if(static AND libstdcxx)
    list(APPEND expected share/licenses/aglow/GPL-3.0-or-later.txt share/licenses/aglow/LICENSE.libstdc++)
endif()
file(GLOB_RECURSE installed LIST_DIRECTORIES false RELATIVE "${prefix}" "${prefix}/*")
# The private libraries of aglow-mux, by their names and sonames: their
# versions are those of Afterglow and of phosphor-channel.
foreach(library AfterglowVt AfterglowPty PhosphorChannel)
    set(found ${installed})
    list(FILTER found INCLUDE REGEX "^${libdir}/afterglow/lib${library}\\.so\\.[0-9.]+$")
    if(NOT found)
        message(FATAL_ERROR "The installation of aglow-mux alone has no lib${library} in ${libdir}/afterglow.")
    endif()
    list(APPEND expected ${found})
endforeach()
list(SORT expected)
list(SORT installed)
if(NOT installed STREQUAL expected)
    set(extra ${installed})
    list(REMOVE_ITEM extra ${expected})
    set(missing ${expected})
    if(installed)
        list(REMOVE_ITEM missing ${installed})
    endif()
    message(FATAL_ERROR "The installation of aglow and aglow-mux alone is not the two programs, the libraries of "
                        "aglow-mux, the completions of aglow and the license notices.\n"
                        "  not expected: ${extra}\n  missing: ${missing}")
endif()

aglow_only_run("installed aglow" "${prefix}/bin/aglow" --version)
if(NOT output MATCHES "^aglow [0-9.]+ \\(protocol [0-9]+\\)\n$")
    message(FATAL_ERROR "The installed aglow printed an unexpected version:\n${output}")
endif()
# The installed aglow-mux finds its private libraries: the system has none of
# them.
aglow_only_run("installed aglow-mux" "${prefix}/bin/aglow-mux" --version)
if(NOT output MATCHES "^aglow-mux [0-9.]+ \\(protocol [0-9]+\\)\n$")
    message(FATAL_ERROR "The installed aglow-mux printed an unexpected version:\n${output}")
endif()
list(LENGTH installed count)
message(STATUS "aglow and aglow-mux alone: configured without Qt, built, and installed as ${count} files")
