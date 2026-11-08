# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(GTest REQUIRED)

# The Render and FontRendering tests draw through the real Vulkan pipeline
# into offscreen images, and skip without a Vulkan device or Wayland display
# (CI without a GPU). On a machine with Vulkan, a device that cannot be made
# fails them. The Snapshot, Bidi, Fonts, GlyphCache and ShaderGeometry tests
# run everywhere.
add_executable(afterglow_render_tests RenderTest.cpp BidiTest.cpp)
target_link_libraries(afterglow_render_tests PRIVATE Afterglow::Render Qt6::Gui GTest::gtest)
set_target_properties(afterglow_render_tests PROPERTIES AUTOMOC OFF)
# The overlay transition pack, as it ships.
target_compile_definitions(afterglow_render_tests PRIVATE
    AFTERGLOW_OVERLAY_FADE_PACK="${PROJECT_SOURCE_DIR}/data/shaders/overlay-fade/effect.frag")

include(GoogleTest)
# Each test process makes its own Vulkan device, with buffers of up to the
# 256 MiB budget of a pack. Several at once ran out of device memory under
# ctest -j, beside the desktop's own use of the GPU: the GPU tests take turns.
gtest_discover_tests(afterglow_render_tests DISCOVERY_MODE PRE_TEST PROPERTIES RESOURCE_LOCK gpu)
