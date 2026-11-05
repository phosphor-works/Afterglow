# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# cmake -DREADELF=<readelf> -DBINARY=<aglow> [-DNOTES_ONLY=ON] -P CheckStatic.cmake
#
# Says what a static BINARY needs from the host that runs it: the oldest Linux
# kernel (the ABI tag of glibc's crt1.o) and the x86-64 ISA level that the
# linked objects ask for. Both come from the static C library of the build
# host, as its distribution configured it, and not from aglow's code. Then,
# without NOTES_ONLY, it fails when the dynamic section of BINARY names a
# shared library.
execute_process(COMMAND "${READELF}" -n -W "${BINARY}" OUTPUT_VARIABLE notes ERROR_QUIET RESULT_VARIABLE result)
if(result EQUAL 0)
    if(notes MATCHES "OS: Linux, ABI: ([0-9.]+)")
        message(STATUS "${BINARY} needs Linux ${CMAKE_MATCH_1} or later")
    endif()
    # The list of levels can share its line with the next property.
    if(notes MATCHES "x86 ISA needed: (x86-64-[a-z0-9]+(, x86-64-[a-z0-9]+)*)")
        string(REGEX MATCH "[^ ,]+$" level "${CMAKE_MATCH_1}")
        message(STATUS "${BINARY} needs an x86-64 processor of level ${level} or later")
    endif()
endif()
if(NOTES_ONLY)
    return()
endif()

execute_process(COMMAND "${READELF}" -d "${BINARY}" OUTPUT_VARIABLE dynamic ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${READELF} -d ${BINARY} failed: ${error}")
endif()
if(dynamic MATCHES "NEEDED")
    message(FATAL_ERROR "${BINARY} is not static:\n${dynamic}")
endif()
message(STATUS "${BINARY} needs no shared library")
