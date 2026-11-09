# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

# The calls of the mux daemon and the values that they carry. The client in
# afterglow and the daemon link the same code.
add_library(afterglow_mux_protocol STATIC protocol/MuxProtocol.cpp)
target_include_directories(afterglow_mux_protocol PUBLIC protocol)
target_link_libraries(afterglow_mux_protocol PUBLIC Afterglow::Vt Afterglow::Proto)
set_target_properties(afterglow_mux_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF)

# afterglow-mux: the daemon that keeps sessions running while no window shows
# them. No Qt: only the terminal core, the PTY and the protocol library.
add_executable(afterglow-mux
    src/Daemon.cpp
    src/MuxPane.cpp
    src/Session.cpp
    src/main.cpp
)
target_compile_definitions(afterglow-mux PRIVATE AFTERGLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(afterglow-mux PRIVATE afterglow_mux_protocol Afterglow::Pty)
# Next to afterglow, which runs the afterglow-mux of its own directory.
set_target_properties(afterglow-mux PROPERTIES AUTOMOC OFF AUTOUIC OFF
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:afterglow>")
install(TARGETS afterglow-mux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
