# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(GTest REQUIRED)

add_executable(afterglow_image_tests DecodeTest.cpp)
target_link_libraries(afterglow_image_tests PRIVATE Afterglow::Image afterglow_image_test_files GTest::gtest_main)
set_target_properties(afterglow_image_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)

include(GoogleTest)
# The decoders run on cut and corrupt files in the test process, with no
# limit on their CPU time as the decoder children of aglow and afterglow-mux
# have: the timeout ends a decoder that does not return.
gtest_discover_tests(afterglow_image_tests TEST_PREFIX "Image." PROPERTIES TIMEOUT 60)
