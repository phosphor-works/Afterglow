# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

# The calls of the mux daemon and the values that they carry. The client in
# afterglow and the daemon link the same code.
add_library(afterglow_mux_protocol STATIC protocol/MuxConnect.cpp protocol/MuxHost.cpp protocol/MuxLayout.cpp protocol/MuxProtocol.cpp
                                         protocol/MuxStart.cpp
                                         protocol/MuxStream.cpp)
target_include_directories(afterglow_mux_protocol PUBLIC protocol)
# phosphor-channel carries the frames of an attached session: no Qt, so the
# daemon links it too.
target_link_libraries(afterglow_mux_protocol PUBLIC Afterglow::Vt Afterglow::Proto PhosphorChannel::PhosphorChannel PRIVATE Afterglow::Pty)
set_target_properties(afterglow_mux_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF)

# The image decoder of the daemon (aglow-mux --decode-images), which the
# tests link too.
add_library(afterglow_mux_image STATIC src/ImageDecoder.cpp)
target_include_directories(afterglow_mux_image PRIVATE src)
target_link_libraries(afterglow_mux_image PUBLIC Afterglow::Proto PRIVATE Afterglow::Image Afterglow::Pty)
set_target_properties(afterglow_mux_image PROPERTIES AUTOMOC OFF AUTOUIC OFF)

# aglow-mux: the daemon that keeps sessions running while no window shows
# them. No Qt: only the terminal core, the PTY, the protocol library and the
# image decoder. The tests of its panes link the same code.
add_library(afterglow_mux_daemon STATIC src/Attach.cpp src/Daemon.cpp src/DaemonCommands.cpp src/DaemonControl.cpp src/MuxPane.cpp src/Proxy.cpp
                                       src/Session.cpp)
target_include_directories(afterglow_mux_daemon PUBLIC src)
target_compile_definitions(afterglow_mux_daemon PRIVATE AFTERGLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(afterglow_mux_daemon PUBLIC afterglow_mux_protocol afterglow_mux_image Afterglow::Pty)
set_target_properties(afterglow_mux_daemon PROPERTIES AUTOMOC OFF AUTOUIC OFF)
add_executable(aglow-mux src/main.cpp)
target_compile_definitions(aglow-mux PRIVATE AFTERGLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(aglow-mux PRIVATE afterglow_mux_daemon)
set_target_properties(aglow-mux PROPERTIES AUTOMOC OFF AUTOUIC OFF)
install(TARGETS aglow-mux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(AFTERGLOW_AGLOW_ONLY)
    # Alone, aglow-mux installs the private libraries that it links, which
    # afterglow installs in a whole build (afterglow/CMakeLists.txt), and the
    # notices of what it holds: its own license and that of the libraries,
    # zlib-ng and xxHash (libs/afterglow-proto) and the image codecs
    # (libs/afterglow-image). They go in a directory named for aglow-mux, as
    # a package of its own has. PCRE2 and the C and C++ runtime libraries
    # are the system's, with their own notices.
    set(AFTERGLOW_MUX_PRIVATE_LIBS AfterglowVt AfterglowPty PhosphorChannel)
    set_target_properties(${AFTERGLOW_MUX_PRIVATE_LIBS} PROPERTIES INSTALL_RPATH "$ORIGIN")
    install(TARGETS ${AFTERGLOW_MUX_PRIVATE_LIBS} LIBRARY DESTINATION ${AFTERGLOW_PRIVATE_LIBDIR} NAMELINK_SKIP)
    set(AFTERGLOW_MUX_LICENSE_DIR "${CMAKE_INSTALL_DATADIR}/licenses/aglow-mux")
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSES/GPL-3.0-or-later.txt" "${PROJECT_SOURCE_DIR}/LICENSES/LGPL-2.1-or-later.txt"
            DESTINATION ${AFTERGLOW_MUX_LICENSE_DIR})
    install(FILES "${AFTERGLOW_ZLIB_LICENSE}" DESTINATION ${AFTERGLOW_MUX_LICENSE_DIR} RENAME LICENSE.zlib-ng)
    install(FILES "${AFTERGLOW_XXHASH_LICENSE}" DESTINATION ${AFTERGLOW_MUX_LICENSE_DIR} RENAME LICENSE.xxHash)
    afterglow_install_image_notices(${AFTERGLOW_MUX_LICENSE_DIR})
else()
    # Next to afterglow, which runs the aglow-mux of its own directory.
    set_target_properties(aglow-mux PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:afterglow>")
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
