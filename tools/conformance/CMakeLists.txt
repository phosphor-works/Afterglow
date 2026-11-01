# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# Runs a program in a pty against the bare terminal core, for conformance
# suites (esctest) that drive the terminal and read its replies.
add_executable(afterglow_headless headless.cpp)
target_link_libraries(afterglow_headless PRIVATE Afterglow::Vt util)
set_target_properties(afterglow_headless PROPERTIES AUTOMOC OFF)

# The runner's own regression checks: argument errors, exit codes and bounds.
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME afterglow_headless_runner
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_headless.py
                    $<TARGET_FILE:afterglow_headless>)
        set_tests_properties(afterglow_headless_runner PROPERTIES TIMEOUT 120)
    endif()
endif()
