# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# Runs a program in a pty against the bare terminal core, for conformance
# suites (esctest) that drive the terminal and read its replies.
add_executable(afterglow_headless headless.cpp)
target_link_libraries(afterglow_headless PRIVATE Afterglow::Vt util)
set_target_properties(afterglow_headless PROPERTIES AUTOMOC OFF)

# Runs a full-screen program against the terminal core with random input and
# checks the screen after each step (docs/TESTING.md, "TUI fuzzing").
add_executable(afterglow_tuifuzz tuifuzz.cpp)
target_link_libraries(afterglow_tuifuzz PRIVATE Afterglow::Vt util)
set_target_properties(afterglow_tuifuzz PROPERTIES AUTOMOC OFF)

# The runner's own regression checks: argument errors, exit codes and bounds.
if(BUILD_TESTING)
    # The checks of afterglow_tuifuzz see a broken screen.
    find_package(GTest REQUIRED)
    add_executable(afterglow_screen_checks_tests ScreenChecksTest.cpp)
    target_link_libraries(afterglow_screen_checks_tests PRIVATE Afterglow::Vt GTest::gtest_main)
    set_target_properties(afterglow_screen_checks_tests PROPERTIES AUTOMOC OFF)
    include(GoogleTest)
    gtest_discover_tests(afterglow_screen_checks_tests)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME afterglow_headless_runner
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_headless.py
                    $<TARGET_FILE:afterglow_headless>)
        set_tests_properties(afterglow_headless_runner PROPERTIES TIMEOUT 120)
        add_test(NAME afterglow_tuifuzz_runner
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_tuifuzz.py
                    $<TARGET_FILE:afterglow_tuifuzz>)
        set_tests_properties(afterglow_tuifuzz_runner PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
        # kitten icat of Kitty against the terminal core, and its output drawn
        # by the renderer (77: no kitten).
        add_test(NAME afterglow_kitten_icat
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/kitten_icat.py
                    $<TARGET_FILE:afterglow_headless> $<TARGET_FILE:afterglow_renderpng>)
        set_tests_properties(afterglow_kitten_icat PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 RESOURCE_LOCK gpu)
    endif()
endif()
