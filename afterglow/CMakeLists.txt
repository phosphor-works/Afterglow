# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Qt6 ${AFTERGLOW_QT_MIN} REQUIRED COMPONENTS Core Gui)

add_executable(afterglow
    src/main.cpp
    src/Pane.cpp
    src/Pane.h
    src/SplitTree.cpp
    src/SplitTree.h
    src/TerminalWindow.cpp
    src/TerminalWindow.h
    src/KeyMapping.cpp
    src/KeyMapping.h
    src/XkbKeyboard.cpp
    src/XkbKeyboard.h
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
target_compile_definitions(afterglow PRIVATE QT_NO_CAST_FROM_ASCII AFTERGLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(afterglow PRIVATE Afterglow::Render Afterglow::Pty Afterglow::Vt Qt6::Core Qt6::Gui
    PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON)

install(TARGETS afterglow RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
