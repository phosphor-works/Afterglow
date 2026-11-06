# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# cmake -DRESULT=<status> -DREGEX=<regex> [-DCOPY_TO=<dir> [-DNOT_EXECUTABLE=ON]] -P RunCheck.cmake -- COMMAND [ARG...]
#
# Runs COMMAND and fails unless it exits with RESULT and its output (stdout
# and stderr together) matches REGEX. PASS_REGULAR_EXPRESSION alone ignores the
# exit status, so a crash after the right text would pass. With COPY_TO, it
# runs a copy of COMMAND made alone in the empty directory COPY_TO, so that
# nothing that COMMAND looks for next to itself is there. With NOT_EXECUTABLE
# too, COPY_TO also holds an empty aglow that nobody may execute.
set(command "")
set(take OFF)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE ${last})
    if(take)
        list(APPEND command "${CMAKE_ARGV${i}}")
    elseif(CMAKE_ARGV${i} STREQUAL "--")
        set(take ON)
    endif()
endforeach()
if(NOT command OR NOT DEFINED RESULT OR NOT DEFINED REGEX)
    message(FATAL_ERROR "usage: cmake -DRESULT=<status> -DREGEX=<regex> -P ${CMAKE_CURRENT_LIST_FILE} -- COMMAND [ARG...]")
endif()
if(COPY_TO)
    file(REMOVE_RECURSE "${COPY_TO}")
    list(POP_FRONT command program)
    file(COPY "${program}" DESTINATION "${COPY_TO}")
    get_filename_component(name "${program}" NAME)
    list(PREPEND command "${COPY_TO}/${name}")
    if(NOT_EXECUTABLE)
        file(WRITE "${COPY_TO}/aglow" "")
        file(CHMOD "${COPY_TO}/aglow" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    endif()
endif()
execute_process(COMMAND ${command} OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE status)
list(JOIN command " " shown)
if(NOT status STREQUAL RESULT)
    message(FATAL_ERROR "${shown} exited with ${status}, not ${RESULT}:\n${output}")
endif()
if(NOT output MATCHES "${REGEX}")
    message(FATAL_ERROR "The output of ${shown} does not match '${REGEX}':\n${output}")
endif()
message(STATUS "${shown}: ${status}, output matches")
