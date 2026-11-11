# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

# The calls of the mux daemon and the values that they carry. The client in
# afterglow and the daemon link the same code.
add_library(afterglow_mux_protocol STATIC protocol/MuxConnect.cpp protocol/MuxHost.cpp protocol/MuxProtocol.cpp protocol/MuxStream.cpp)
target_include_directories(afterglow_mux_protocol PUBLIC protocol)
# phosphor-channel carries the frames of an attached session: no Qt, so the
# daemon links it too.
target_link_libraries(afterglow_mux_protocol PUBLIC Afterglow::Vt Afterglow::Proto PhosphorChannel::PhosphorChannel)
set_target_properties(afterglow_mux_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF)

# The image decoder of the daemon: the patched stb_image and the GIF reader
# of aglow, built as aglow builds them (tools/aglow/CMakeLists.txt): stb's
# code without Afterglow's warnings, and with -fwrapv. aglow can be off, so
# the source is compiled here again, as tools/fuzz does.
afterglow_vendored_file(stb_image AFTERGLOW_MUX_STB_IMAGE)
get_filename_component(AFTERGLOW_MUX_STB_DIR "${AFTERGLOW_MUX_STB_IMAGE}" DIRECTORY)
set(AFTERGLOW_MUX_STB_SOURCE "${PROJECT_SOURCE_DIR}/tools/aglow/StbImage.cpp")
add_library(afterglow_mux_image STATIC src/ImageDecoder.cpp "${AFTERGLOW_MUX_STB_SOURCE}")
target_include_directories(afterglow_mux_image PRIVATE src "${PROJECT_SOURCE_DIR}/tools/aglow")
target_include_directories(afterglow_mux_image SYSTEM PRIVATE "${AFTERGLOW_MUX_STB_DIR}")
set_source_files_properties("${AFTERGLOW_MUX_STB_SOURCE}" TARGET_DIRECTORY afterglow_mux_image
    PROPERTIES COMPILE_OPTIONS "-w;-fwrapv")
target_link_libraries(afterglow_mux_image PUBLIC Afterglow::Proto)
set_target_properties(afterglow_mux_image PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF AUTOMOC OFF AUTOUIC OFF)
# stb_image's MIT notice goes with the daemon. aglow installs the same file
# in the same directory when it is built.
if(NOT AFTERGLOW_BUILD_AGLOW)
    afterglow_stb_image_notice("${CMAKE_CURRENT_BINARY_DIR}/LICENSE.stb_image")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.stb_image" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow)
endif()

# afterglow-mux: the daemon that keeps sessions running while no window shows
# them. No Qt: only the terminal core, the PTY, the protocol library and the
# image decoder. The tests of its panes link the same code.
add_library(afterglow_mux_daemon STATIC src/Attach.cpp src/Daemon.cpp src/MuxPane.cpp src/Session.cpp)
target_include_directories(afterglow_mux_daemon PUBLIC src)
target_compile_definitions(afterglow_mux_daemon PRIVATE AFTERGLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(afterglow_mux_daemon PUBLIC afterglow_mux_protocol afterglow_mux_image Afterglow::Pty)
set_target_properties(afterglow_mux_daemon PROPERTIES AUTOMOC OFF AUTOUIC OFF)
add_executable(afterglow-mux src/main.cpp)
target_compile_definitions(afterglow-mux PRIVATE AFTERGLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(afterglow-mux PRIVATE afterglow_mux_daemon)
# Next to afterglow, which runs the afterglow-mux of its own directory.
set_target_properties(afterglow-mux PROPERTIES AUTOMOC OFF AUTOUIC OFF
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:afterglow>")
install(TARGETS afterglow-mux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
