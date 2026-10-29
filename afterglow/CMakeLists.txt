# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Qt6 ${AFTERGLOW_QT_MIN} REQUIRED COMPONENTS Core Gui Qml Quick)
find_package(Qt6GuiPrivate CONFIG REQUIRED) # rhi/qrhi.h for the chrome render target
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

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
    src/config/Keybinds.cpp
    src/config/Keybinds.h
    src/config/Settings.cpp
    src/config/Settings.h
    src/chrome/ChromeController.cpp
    src/chrome/ChromeController.h
    src/chrome/ChromeLayer.cpp
    src/chrome/ChromeLayer.h
)

qt_add_qml_module(afterglow
    URI Afterglow.Chrome
    VERSION 1.0
    QML_FILES
        qml/Chrome.qml
        qml/CommandPalette.qml
        qml/SearchBar.qml
        qml/TabBar.qml
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
target_compile_definitions(afterglow PRIVATE QT_NO_CAST_FROM_ASCII AFTERGLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(afterglow PRIVATE Afterglow::Render Afterglow::Pty Afterglow::Vt Qt6::Core Qt6::Gui
    Qt6::Qml Qt6::Quick Qt6::GuiPrivate
    PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON PhosphorConfig::PhosphorConfig)

install(TARGETS afterglow RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
