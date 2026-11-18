# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(afterglow_renderpng renderpng.cpp)
# Links the application's shader loading (GPL), so packs load as in the app,
# its overlay options, so a popup is placed by the window's rules, the
# decoders of libs/afterglow-image, so images decode with the formats and
# limits of a pane's decoding process (here in this process), and its
# settings, for the defaults of the window's look.
target_link_libraries(afterglow_renderpng PRIVATE afterglow_shaders afterglow_overlayspec afterglow_config
    Afterglow::Image Afterglow::Proto Afterglow::Render Qt6::Gui)

# iTerm2 images decode as in a pane, and animations move on with --frames
# (77: no Vulkan).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME afterglow_renderpng_images
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/renderpng_images.py $<TARGET_FILE:afterglow_renderpng>)
    set_tests_properties(afterglow_renderpng_images PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 RESOURCE_LOCK gpu)
endif()
