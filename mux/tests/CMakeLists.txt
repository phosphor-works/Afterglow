# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(GTest REQUIRED)

add_executable(afterglow_mux_tests DaemonTest.cpp MuxProtocolTest.cpp)
target_link_libraries(afterglow_mux_tests PRIVATE afterglow_mux_protocol GTest::gtest_main)
# Each test runs its own daemon, with a runtime directory of its own.
target_compile_definitions(afterglow_mux_tests PRIVATE AFTERGLOW_MUX="$<TARGET_FILE:afterglow-mux>")
add_dependencies(afterglow_mux_tests afterglow-mux)
set_target_properties(afterglow_mux_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)

include(GoogleTest)
gtest_discover_tests(afterglow_mux_tests TEST_PREFIX "Mux.")
