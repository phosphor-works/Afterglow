# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# The image decoders of aglow and afterglow-mux (no Qt): the system's
# libpng, libjpeg-turbo (the TurboJPEG 3 API) and libwebp, and GIF and BMP
# readers of its own. Qt decodes with the same libraries in the window. The
# imported target is global: tools/fuzz compiles the sources again.
find_package(PkgConfig REQUIRED)
pkg_check_modules(AFTERGLOW_IMAGE_CODECS REQUIRED IMPORTED_TARGET GLOBAL libpng libturbojpeg>=3.0 libwebp libwebpdemux)

add_library(AfterglowImage STATIC
    src/Bmp.cpp
    src/Decode.cpp
    src/Gif.cpp
    src/Jpeg.cpp
    src/Png.cpp
    src/WebP.cpp
)
add_library(Afterglow::Image ALIAS AfterglowImage)
target_include_directories(AfterglowImage PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(AfterglowImage PRIVATE PkgConfig::AFTERGLOW_IMAGE_CODECS)
set_target_properties(AfterglowImage PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
