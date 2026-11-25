# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(GTest REQUIRED)

add_executable(afterglow_mux_tests AttachTest.cpp DaemonTest.cpp ImageDecoderTest.cpp MuxLayoutTest.cpp MuxPaneTest.cpp MuxProtocolTest.cpp
               MuxStartTest.cpp ProxyTest.cpp)
# The image files of the tests of the decoders (libs/afterglow-image).
target_link_libraries(afterglow_mux_tests PRIVATE afterglow_mux_daemon afterglow_image_test_files GTest::gtest_main)
afterglow_test_uses_jthread(afterglow_mux_tests)
# A test that runs a daemon gives it a runtime directory, a state directory and a
# home directory of its own, so that it never reaches the user's daemon.
afterglow_test_runs(afterglow_mux_tests AFTERGLOW_MUX aglow-mux)
set_target_properties(afterglow_mux_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)

include(GoogleTest)
# Each test bounds its own waits; the timeout ends a test that hangs anyway.
gtest_discover_tests(afterglow_mux_tests TEST_PREFIX "Mux." PROPERTIES TIMEOUT 60)
