# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(Qt6 ${AFTERGLOW_QT_MIN} REQUIRED COMPONENTS Core Gui ShaderTools)
find_package(Qt6GuiPrivate CONFIG REQUIRED)
find_package(Qt6ShaderToolsPrivate CONFIG REQUIRED) # QShaderBaker for terminal shaders
find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(FRIBIDI REQUIRED IMPORTED_TARGET fribidi)

qt_add_library(AfterglowRender SHARED
    src/FontSet.cpp
    src/GlyphCache.cpp
    src/GpuDevice.cpp
    src/GridRenderer.cpp
    src/PostChain.cpp
    src/ShaderProgram.cpp
    src/Snapshot.cpp
    src/Bidi.cpp
    src/Sprites.cpp
    src/Sprites.h
    include/AfterglowRender/RenderFrame.h
    include/AfterglowRender/GpuDevice.h
)
add_library(Afterglow::Render ALIAS AfterglowRender)

include(GenerateExportHeader)
generate_export_header(AfterglowRender
    BASE_NAME AfterglowRender
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/AfterglowRender/afterglowrender_export.h)

target_include_directories(AfterglowRender PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
target_compile_definitions(AfterglowRender PRIVATE QT_NO_CAST_FROM_ASCII)
target_link_libraries(AfterglowRender
    # Public headers include the QRhi headers (rhi/qrhi.h).
    PUBLIC Afterglow::Vt PhosphorShaders::PhosphorShaders Qt6::Core Qt6::Gui Qt6::GuiPrivate
    PRIVATE Qt6::ShaderToolsPrivate PkgConfig::FREETYPE PkgConfig::HARFBUZZ PkgConfig::FONTCONFIG PkgConfig::FRIBIDI)
# Qt 6.12.0 can leave the Wayland surface locked when Vulkan beginFrame fails.
# Inspect the lock state so a distribution backport does not double-unlock it.
if(Qt6_VERSION VERSION_EQUAL "6.12.0")
    find_package(Qt6 ${AFTERGLOW_QT_MIN} REQUIRED COMPONENTS WaylandClient)
    find_package(Qt6WaylandClientPrivate CONFIG REQUIRED)
    target_link_libraries(AfterglowRender PRIVATE Qt6::WaylandClientPrivate)
    target_compile_definitions(AfterglowRender PRIVATE AFTERGLOW_QT_612_FRAME_RECOVERY)
endif()

set_target_properties(AfterglowRender PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

# Vulkan only: SPIR-V, no GLSL/HLSL/MSL variants.
qt_add_shaders(AfterglowRender "afterglow_grid_shaders"
    PREFIX "/afterglow"
    NOGLSL NOHLSL NOMSL
    FILES
        shaders/grid.vert
        shaders/grid.frag
        shaders/image.frag
        shaders/layers.frag
        shaders/post.vert
)

# The terminal shader contract, which ShaderProgram completes every pack with.
# Its source of truth is data/shaders/shared, where pack authors read it.
set(AFTERGLOW_TERMINAL_CONTRACT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../data/shaders/shared")
qt_add_resources(AfterglowRender "afterglow_terminal_contract"
    PREFIX "/afterglow/shaders"
    BASE "${AFTERGLOW_TERMINAL_CONTRACT_DIR}"
    FILES
        ${AFTERGLOW_TERMINAL_CONTRACT_DIR}/terminal_audio.glsl
        ${AFTERGLOW_TERMINAL_CONTRACT_DIR}/terminal_color.glsl
        ${AFTERGLOW_TERMINAL_CONTRACT_DIR}/terminal_history.glsl
        ${AFTERGLOW_TERMINAL_CONTRACT_DIR}/terminal_lib.glsl
        ${AFTERGLOW_TERMINAL_CONTRACT_DIR}/terminal_multipass.glsl
        ${AFTERGLOW_TERMINAL_CONTRACT_DIR}/terminal_uniforms.glsl
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
