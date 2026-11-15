# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(GTest REQUIRED)

add_executable(afterglow_mux_tests AttachTest.cpp DaemonTest.cpp ImageDecoderTest.cpp MuxLayoutTest.cpp MuxPaneTest.cpp MuxProtocolTest.cpp)
# The image files of the tests of the decoders (libs/afterglow-image).
target_link_libraries(afterglow_mux_tests PRIVATE afterglow_mux_daemon afterglow_image_test_files GTest::gtest_main)
# A test that runs a daemon gives it a runtime directory, a state directory and a
# home directory of its own, so that it never reaches the user's daemon.
target_compile_definitions(afterglow_mux_tests PRIVATE AFTERGLOW_MUX="$<TARGET_FILE:afterglow-mux>")
add_dependencies(afterglow_mux_tests afterglow-mux)
set_target_properties(afterglow_mux_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)

include(GoogleTest)
# Each test bounds its own waits; the timeout ends a test that hangs anyway.
gtest_discover_tests(afterglow_mux_tests TEST_PREFIX "Mux." PROPERTIES TIMEOUT 60)
