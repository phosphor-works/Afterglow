# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(GTest REQUIRED)

add_executable(afterglow_pty_tests PtyTest.cpp)
target_link_libraries(afterglow_pty_tests PRIVATE Afterglow::Pty GTest::gtest_main)
set_target_properties(afterglow_pty_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)

include(GoogleTest)
# The application's own pty tests are Pty.*: the prefix keeps the ctest names apart.
gtest_discover_tests(afterglow_pty_tests TEST_PREFIX "PtyLib.")
