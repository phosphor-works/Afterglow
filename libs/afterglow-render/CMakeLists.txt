# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(Qt6 ${AFTERGLOW_QT_MIN} REQUIRED COMPONENTS Core Gui ShaderTools)
find_package(Qt6GuiPrivate CONFIG REQUIRED)
find_package(Qt6ShaderToolsPrivate CONFIG REQUIRED) # QShaderBaker for terminal shaders
find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

add_library(AfterglowRender SHARED
    src/FontSet.cpp
    src/GlyphCache.cpp
    src/GpuDevice.cpp
    src/GridRenderer.cpp
    src/PostChain.cpp
    src/ShaderProgram.cpp
    src/Snapshot.cpp
    src/Sprites.cpp
    src/Sprites.h
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
    PUBLIC Afterglow::Vt Qt6::Core Qt6::Gui Qt6::GuiPrivate
    PRIVATE Qt6::ShaderToolsPrivate PkgConfig::FREETYPE PkgConfig::HARFBUZZ PkgConfig::FONTCONFIG)
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
        shaders/post.vert
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
