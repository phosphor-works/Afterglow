# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# cmake -DREADELF=<readelf> -DBINARY=<aglow> -P CheckStatic.cmake
# Fails when the dynamic section of BINARY names a shared library.
execute_process(COMMAND "${READELF}" -d "${BINARY}" OUTPUT_VARIABLE dynamic ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${READELF} -d ${BINARY} failed: ${error}")
endif()
if(dynamic MATCHES "NEEDED")
    message(FATAL_ERROR "${BINARY} is not static:\n${dynamic}")
endif()
message(STATUS "${BINARY} needs no shared library")
