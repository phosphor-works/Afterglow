# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(GTest REQUIRED)

# The Render, FontRendering and PopupRender tests draw through the real
# Vulkan pipeline into offscreen images. They skip without a Wayland display
# or without a Vulkan 1.2 device (a software driver such as lavapipe is
# one). On a machine with Vulkan, a device that cannot be made fails them.
# The Snapshot, SnapshotCursor, SnapshotImages, Bidi, Fonts, GlyphCache,
# ShaderGeometry and Readback tests run everywhere.
add_executable(afterglow_render_tests RenderTest.cpp BidiTest.cpp GpuTestDevice.h)
target_link_libraries(afterglow_render_tests PRIVATE Afterglow::Render Qt6::Gui GTest::gtest)
set_target_properties(afterglow_render_tests PROPERTIES AUTOMOC OFF)
# Strings as the renderer builds them: no QString from a bare const char*.
target_compile_definitions(afterglow_render_tests PRIVATE QT_NO_CAST_FROM_ASCII)
# The overlay transition pack, as it ships.
target_compile_definitions(afterglow_render_tests PRIVATE
    AFTERGLOW_OVERLAY_FADE_PACK="${PROJECT_SOURCE_DIR}/data/shaders/overlay-fade/effect.frag")

include(GoogleTest)
# Each test process makes its own Vulkan device, with buffers of up to the
# 256 MiB budget of a pack. Several at once ran out of device memory under
# ctest -j, beside the desktop's own use of the GPU: the GPU tests take turns.
# The lock covers every test of the binary, the few that draw nothing too:
# they take milliseconds. A test that hangs on the GPU gives the lock back
# at its timeout; one takes about a second.
gtest_discover_tests(afterglow_render_tests DISCOVERY_MODE PRE_TEST PROPERTIES RESOURCE_LOCK gpu TIMEOUT 60)
