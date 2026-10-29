# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Qt6 ${AFTERGLOW_QT_MIN} REQUIRED COMPONENTS Core Gui Qml Quick DBus)
find_package(KF6WindowSystem REQUIRED) # KWin blur behind
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
    src/animation/FrameClock.cpp
    src/animation/FrameClock.h
    src/animation/PaneMotion.h
    src/app/Application.cpp
    src/app/Application.h
    src/app/Services.h
)

# Shader pack loading, shared with the developer render tool.
add_library(afterglow_shaders STATIC src/shaders/ShaderLibrary.cpp src/shaders/ShaderLibrary.h)
target_include_directories(afterglow_shaders PUBLIC src)
target_compile_definitions(afterglow_shaders PRIVATE QT_NO_CAST_FROM_ASCII
    AFTERGLOW_SOURCE_SHADER_DIR="${PROJECT_SOURCE_DIR}/data/shaders")
target_link_libraries(afterglow_shaders PUBLIC Afterglow::Render PhosphorShaders::PhosphorShaders Qt6::Core)

qt_add_qml_module(afterglow
    URI Afterglow.Chrome
    VERSION 1.0
    QML_FILES
        qml/Chrome.qml
        qml/CommandPalette.qml
        qml/PasteConfirm.qml
        qml/SearchBar.qml
        qml/TabBar.qml
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
target_compile_definitions(afterglow PRIVATE QT_NO_CAST_FROM_ASCII AFTERGLOW_VERSION="${PROJECT_VERSION}"
    AFTERGLOW_BUILD_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins"
    AFTERGLOW_BUILD_TERMINFO_DIR="${CMAKE_BINARY_DIR}/share/terminfo"
    AFTERGLOW_INSTALLED_TERMINFO_DIR="${CMAKE_INSTALL_FULL_DATADIR}/terminfo")
# The layer-shell QPA plugin is part of the phosphor build; make sure it exists.
add_dependencies(afterglow phosphorwayland-qpa)
target_link_libraries(afterglow PRIVATE afterglow_shaders Afterglow::Render Afterglow::Pty Afterglow::Vt Qt6::Core Qt6::Gui
    Qt6::Qml Qt6::Quick Qt6::GuiPrivate
    PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON PhosphorConfig::PhosphorConfig PhosphorShaders::PhosphorShaders
    PhosphorAnimation::PhosphorAnimation PhosphorShortcuts::PhosphorShortcuts PhosphorWayland::PhosphorWayland
    Qt6::DBus KF6::WindowSystem)

install(TARGETS afterglow RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/shaders/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/shaders)

# The afterglow terminfo entry, compiled into the build tree for development
# runs and installed under share/terminfo.
find_program(TIC_EXECUTABLE tic)
if(TIC_EXECUTABLE)
    set(AFTERGLOW_TERMINFO_BUILD_DIR ${CMAKE_BINARY_DIR}/share/terminfo)
    add_custom_command(
        OUTPUT ${AFTERGLOW_TERMINFO_BUILD_DIR}/a/afterglow
        COMMAND ${CMAKE_COMMAND} -E make_directory ${AFTERGLOW_TERMINFO_BUILD_DIR}
        COMMAND ${TIC_EXECUTABLE} -x -o ${AFTERGLOW_TERMINFO_BUILD_DIR} ${PROJECT_SOURCE_DIR}/data/terminfo/afterglow.terminfo
        DEPENDS ${PROJECT_SOURCE_DIR}/data/terminfo/afterglow.terminfo
        COMMENT "Compiling the afterglow terminfo entry")
    add_custom_target(afterglow_terminfo ALL DEPENDS ${AFTERGLOW_TERMINFO_BUILD_DIR}/a/afterglow)
    add_dependencies(afterglow afterglow_terminfo)
    install(DIRECTORY ${AFTERGLOW_TERMINFO_BUILD_DIR}/ DESTINATION ${CMAKE_INSTALL_DATADIR}/terminfo)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
