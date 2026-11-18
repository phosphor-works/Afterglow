# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

# The calls of the mux daemon and the values that they carry, the attach
# stream, and how a client reaches the daemon or starts it (MuxConnect,
# MuxStart). The client in afterglow and the daemon link the same code.
add_library(afterglow_mux_protocol STATIC
    protocol/MuxConnect.cpp
    protocol/MuxHost.cpp
    protocol/MuxLayout.cpp
    protocol/MuxProtocol.cpp
    protocol/MuxStart.cpp
    protocol/MuxStream.cpp)
target_include_directories(afterglow_mux_protocol PUBLIC protocol)
# MuxStart.cpp includes only the inline AfterglowPty/CloseFrom.h: its include
# directory, not a link, so that every program that links this library does
# not link libAfterglowPty too.
target_include_directories(afterglow_mux_protocol PRIVATE $<TARGET_PROPERTY:AfterglowPty,INTERFACE_INCLUDE_DIRECTORIES>)
# MuxConnect.cpp starts a thread. glibc has the threads in libc; FreeBSD's
# libc does not.
find_package(Threads REQUIRED)
target_link_libraries(afterglow_mux_protocol PUBLIC Afterglow::Vt Afterglow::Proto PRIVATE Threads::Threads)
set_target_properties(afterglow_mux_protocol PROPERTIES AUTOMOC OFF AUTOUIC OFF)

# The image decoder: its client (ImageDecoder.cpp), which the daemon, the
# window's panes and the tests link, and its server (ImageServer.cpp,
# aglow-mux --decode-images), which only aglow-mux links. The client's header
# is alone in include/: a program that links the client sees none of the
# daemon's. The client checks an image's signature (the inline
# AfterglowImage::format) with the include directory of the decoders, not a
# link: the decoders run in the server's processes only, never in a program
# that links the client.
add_library(afterglow_mux_image STATIC src/ImageDecoder.cpp)
target_include_directories(afterglow_mux_image PUBLIC include
                           PRIVATE src $<TARGET_PROPERTY:AfterglowImage,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(afterglow_mux_image PUBLIC Afterglow::Proto)
set_target_properties(afterglow_mux_image PROPERTIES AUTOMOC OFF AUTOUIC OFF)
add_library(afterglow_mux_image_server STATIC src/ImageServer.cpp)
target_include_directories(afterglow_mux_image_server PRIVATE src include)
# With any sanitizer, the decoding processes keep the daemon's standard error
# for the reports. GCC defines no macro for UBSan alone.
afterglow_sanitized(AFTERGLOW_MUX_SANITIZED)
if(AFTERGLOW_MUX_SANITIZED)
    target_compile_definitions(afterglow_mux_image_server PRIVATE AFTERGLOW_MUX_SANITIZED=1)
endif()
target_link_libraries(afterglow_mux_image_server PUBLIC Afterglow::Proto PRIVATE Afterglow::Image Afterglow::Pty)
set_target_properties(afterglow_mux_image_server PROPERTIES AUTOMOC OFF AUTOUIC OFF)

# aglow-mux: the daemon that keeps sessions running while no window shows
# them, and the proxy that afterglow mux --ssh runs on another host
# (Proxy.cpp). It answers afterglow ctl and its watches for its sessions, and
# passes the script commands and the remote commands of its session panes to
# the window that shows them (Daemon*.cpp). No Qt: the terminal core, the
# PTY, the protocol library, the image decoder and phosphor-channel, which
# carries the frames of an attached session (Attach.h, so PUBLIC). The tests
# of its panes link the same code.
add_library(afterglow_mux_daemon STATIC
    src/Attach.cpp
    src/Daemon.cpp
    src/DaemonCommands.cpp
    src/DaemonControl.cpp
    src/DaemonJoin.cpp
    src/DaemonRemote.cpp
    src/DaemonWatch.cpp
    src/KeptCalls.cpp
    src/MuxPane.cpp
    src/Proxy.cpp
    src/Session.cpp)
target_include_directories(afterglow_mux_daemon PUBLIC src)
target_compile_definitions(afterglow_mux_daemon PRIVATE AFTERGLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(afterglow_mux_daemon PUBLIC afterglow_mux_protocol afterglow_mux_image Afterglow::Pty
                      PhosphorChannel::PhosphorChannel)
set_target_properties(afterglow_mux_daemon PROPERTIES AUTOMOC OFF AUTOUIC OFF)
add_executable(aglow-mux src/main.cpp)
target_compile_definitions(aglow-mux PRIVATE AFTERGLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(aglow-mux PRIVATE afterglow_mux_daemon afterglow_mux_image_server)
set_target_properties(aglow-mux PROPERTIES AUTOMOC OFF AUTOUIC OFF)
install(TARGETS aglow-mux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(AFTERGLOW_AGLOW_ONLY)
    # Alone, aglow-mux installs the private libraries that it links, which
    # afterglow installs in a whole build (afterglow/CMakeLists.txt), and the
    # notices of what it holds: its own license and those of the code and
    # data built into it (afterglow_install_bundled_notices). They go in a
    # directory named for aglow-mux, as a package of its own has. PCRE2 and
    # the C and C++ runtime libraries are the system's, with their own
    # notices. PhosphorChannel comes from a directory added EXCLUDE_FROM_ALL
    # (cmake/AfterglowPhosphor.cmake): aglow-mux links it, so the build makes
    # it before the installation, as CMake asks of such a target.
    set_target_properties(${AFTERGLOW_MUX_PRIVATE_LIBS} PROPERTIES INSTALL_RPATH "$ORIGIN")
    install(TARGETS ${AFTERGLOW_MUX_PRIVATE_LIBS} LIBRARY DESTINATION ${AFTERGLOW_PRIVATE_LIBDIR} NAMELINK_SKIP)
    set(AFTERGLOW_MUX_LICENSE_DIR "${CMAKE_INSTALL_DATADIR}/licenses/aglow-mux")
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSES/GPL-3.0-or-later.txt" "${PROJECT_SOURCE_DIR}/LICENSES/LGPL-2.1-or-later.txt"
            DESTINATION ${AFTERGLOW_MUX_LICENSE_DIR})
    afterglow_install_bundled_notices(${AFTERGLOW_MUX_LICENSE_DIR})
else()
    # Next to afterglow, which runs the aglow-mux of its own directory.
    set_target_properties(aglow-mux PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:afterglow>")
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
