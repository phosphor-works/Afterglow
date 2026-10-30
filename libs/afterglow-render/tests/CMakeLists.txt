# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(GTest REQUIRED)

# Renders through the real Vulkan pipeline into offscreen images. Skips when
# there is no Vulkan device or Wayland display (CI without a GPU).
add_executable(afterglow_render_tests RenderTest.cpp BidiTest.cpp)
target_link_libraries(afterglow_render_tests PRIVATE Afterglow::Render Qt6::Gui GTest::gtest)
set_target_properties(afterglow_render_tests PROPERTIES AUTOMOC OFF)

include(GoogleTest)
gtest_discover_tests(afterglow_render_tests DISCOVERY_MODE PRE_TEST)
