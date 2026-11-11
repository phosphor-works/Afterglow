# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(GTest REQUIRED)

add_executable(afterglow_image_tests DecodeTest.cpp)
target_link_libraries(afterglow_image_tests PRIVATE Afterglow::Image GTest::gtest_main)
set_target_properties(afterglow_image_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)

include(GoogleTest)
gtest_discover_tests(afterglow_image_tests TEST_PREFIX "Image.")
