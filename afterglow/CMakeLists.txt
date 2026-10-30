# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Qt6 ${AFTERGLOW_QT_MIN} REQUIRED COMPONENTS Core Gui Widgets Qml Quick QuickControls2 DBus)
find_package(KF6WindowSystem REQUIRED) # KWin blur behind
find_package(Qt6GuiPrivate CONFIG REQUIRED) # rhi/qrhi.h for the chrome render target
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(afterglow
    src/main.cpp
    src/Pane.cpp
    src/Pane.h
    src/SplitTree.cpp
    src/SplitTree.h
    src/TerminalWindow.cpp
    src/TerminalWindow.h
    src/KeyMapping.cpp
    src/KeyMapping.h
    src/chrome/ChromeController.cpp
    src/chrome/ChromeController.h
    src/chrome/ChromeLayer.cpp
    src/chrome/ChromeLayer.h
    src/chrome/IconProvider.cpp
    src/chrome/IconProvider.h
    src/animation/FrameClock.cpp
    src/animation/FrameClock.h
    src/animation/PaneMotion.h
    src/app/Application.cpp
    src/app/NotificationCenter.cpp
    src/control/Control.cpp
    src/control/Control.h
    src/control/ControlBus.cpp
    src/control/ControlBus.h
    src/control/Ctl.cpp
    src/control/Ctl.h
    src/scripting/ScriptControl.cpp
    src/scripting/ScriptControl.h
    src/app/NotificationCenter.h
    src/app/Application.h
    src/app/Services.h
)

# Shader pack loading, shared with the developer render tool.
qt_add_library(afterglow_shaders STATIC src/shaders/ShaderLibrary.cpp src/shaders/ShaderLibrary.h)
target_include_directories(afterglow_shaders PUBLIC src)
target_compile_definitions(afterglow_shaders PRIVATE QT_NO_CAST_FROM_ASCII
    AFTERGLOW_SOURCE_SHADER_DIR="${PROJECT_SOURCE_DIR}/data/shaders")
target_link_libraries(afterglow_shaders PUBLIC Afterglow::Render PhosphorShaders::PhosphorShaders Qt6::Core)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

# Settings, keybinds and themes, shared with their tests.
qt_add_library(afterglow_config STATIC
    src/config/Keybinds.cpp
    src/config/Keybinds.h
    src/config/KeyModes.cpp
    src/config/KeyModes.h
    src/config/Settings.cpp
    src/config/Settings.h
    src/config/Theme.cpp
    src/config/Theme.h
    src/XkbKeyboard.cpp
    src/XkbKeyboard.h)
target_include_directories(afterglow_config PUBLIC src)
target_compile_definitions(afterglow_config PRIVATE QT_NO_CAST_FROM_ASCII
    AFTERGLOW_SOURCE_THEME_DIR="${PROJECT_SOURCE_DIR}/data/themes")
target_link_libraries(afterglow_config PUBLIC afterglow_shaders Afterglow::Render Afterglow::Vt
    PhosphorConfig::PhosphorConfig Qt6::Core Qt6::Gui PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON)

qt_add_library(afterglow_shell STATIC src/shell/ShellIntegration.cpp src/shell/ShellIntegration.h)
target_include_directories(afterglow_shell PUBLIC src)
target_link_libraries(afterglow_shell PUBLIC Afterglow::Pty Qt6::Core)
target_compile_definitions(afterglow_shell PRIVATE QT_NO_CAST_FROM_ASCII
    AFTERGLOW_BUILD_DIR="${CMAKE_BINARY_DIR}"
    AFTERGLOW_SOURCE_SHELL_DIR="${PROJECT_SOURCE_DIR}/data/shell-integration"
    AFTERGLOW_INSTALLED_SHELL_DIR="${CMAKE_INSTALL_FULL_DATADIR}/afterglow/shell-integration")
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/shell-integration/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/shell-integration)

# A single lazy system-playback analyzer, shared with its lifecycle tests.
qt_add_library(afterglow_audio STATIC
    src/audio/AudioController.cpp src/audio/AudioController.h src/audio/AudioSettings.h)
target_include_directories(afterglow_audio PUBLIC src)
target_compile_definitions(afterglow_audio PRIVATE QT_NO_CAST_FROM_ASCII)
target_link_libraries(afterglow_audio PUBLIC PhosphorAudio::PhosphorAudio Qt6::Core)

# The script host, shared with its tests. The Control binding lives in the app.
qt_add_library(afterglow_scripting STATIC src/scripting/ScriptHost.cpp src/scripting/ScriptHost.h)
target_include_directories(afterglow_scripting PUBLIC src)
target_compile_definitions(afterglow_scripting PRIVATE QT_NO_CAST_FROM_ASCII)
target_link_libraries(afterglow_scripting PUBLIC PhosphorScripting::PhosphorScripting Qt6::Core)
qt_add_resources(afterglow_scripting afterglow_scripting_prelude
    PREFIX /afterglow/scripting
    BASE src/scripting
    FILES src/scripting/afterglow.luau)

qt_add_qml_module(afterglow
    URI Afterglow.Chrome
    VERSION 1.0
    QML_FILES
        qml/Chrome.qml
        qml/ClipboardPrompt.qml
        qml/CommandPalette.qml
        qml/ConfirmPrompt.qml
        qml/ContextMenu.qml
        qml/PasteConfirm.qml
        qml/SearchBar.qml
        qml/StatusIndicator.qml
        qml/TitlePrompt.qml
        qml/TabBar.qml
)

target_compile_definitions(afterglow PRIVATE QT_NO_CAST_FROM_ASCII AFTERGLOW_VERSION="${PROJECT_VERSION}"
    AFTERGLOW_BUILD_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins"
    AFTERGLOW_INSTALLED_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/afterglow/plugins"
    AFTERGLOW_BUILD_TERMINFO_DIR="${CMAKE_BINARY_DIR}/share/terminfo"
    AFTERGLOW_INSTALLED_TERMINFO_DIR="${CMAKE_INSTALL_FULL_DATADIR}/terminfo")
# The layer-shell QPA plugin is part of the phosphor build; make sure it exists.
add_dependencies(afterglow phosphorwayland-qpa)
target_link_libraries(afterglow PRIVATE afterglow_shell afterglow_config afterglow_shaders afterglow_scripting afterglow_audio Afterglow::Render Afterglow::Pty Afterglow::Vt Qt6::Core Qt6::Gui
    Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::GuiPrivate
    PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON PhosphorConfig::PhosphorConfig PhosphorShaders::PhosphorShaders
    PhosphorDBus::PhosphorDBus PhosphorScripting::PhosphorScripting
    PhosphorAnimation::PhosphorAnimation PhosphorShortcuts::PhosphorShortcuts PhosphorWayland::PhosphorWayland
    Qt6::DBus KF6::WindowSystem)

install(TARGETS afterglow RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Private libraries: afterglow's own and the phosphor ones it was built with.
install(TARGETS AfterglowVt AfterglowRender AfterglowPty
                PhosphorConfig PhosphorFsLoader PhosphorRegistry PhosphorShaders PhosphorRendering
                PhosphorWayland PhosphorLayer PhosphorAnimation PhosphorShortcuts
                PhosphorDBus PhosphorScripting PhosphorAudio
        LIBRARY DESTINATION ${AFTERGLOW_PRIVATE_LIBDIR} NAMELINK_SKIP)
install(TARGETS phosphorwayland-qpa
        LIBRARY DESTINATION ${AFTERGLOW_PRIVATE_LIBDIR}/plugins/wayland-shell-integration)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/shaders/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/shaders)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/themes/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/themes)
# Scripting: types for editors and examples to copy; neither loads on its own.
install(FILES src/scripting/afterglow.d.luau DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/api)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/examples/scripts/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/examples/scripts)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/examples/glasshouse/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/examples/glasshouse
        USE_SOURCE_PERMISSIONS)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/examples/phosphor/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/examples/phosphor
        USE_SOURCE_PERMISSIONS)

# Desktop integration. The app id matches QGuiApplication::desktopFileName,
# which Wayland uses to find the icon and group windows.
set(AFTERGLOW_APP_ID io.github.fuddlesworth.Afterglow)
install(FILES ${PROJECT_SOURCE_DIR}/data/${AFTERGLOW_APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${PROJECT_SOURCE_DIR}/data/${AFTERGLOW_APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES ${PROJECT_SOURCE_DIR}/data/icons/${AFTERGLOW_APP_ID}.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
# Rendered sizes too: the glow uses SVG filters that not every icon loader draws.
find_program(RSVG_CONVERT rsvg-convert)
if(RSVG_CONVERT)
    set(_icon_pngs)
    foreach(size 16 22 24 32 48 64 128 256 512)
        set(_png ${CMAKE_BINARY_DIR}/icons/${size}x${size}/${AFTERGLOW_APP_ID}.png)
        add_custom_command(OUTPUT ${_png}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/icons/${size}x${size}
            COMMAND ${RSVG_CONVERT} -w ${size} -h ${size} -o ${_png} ${PROJECT_SOURCE_DIR}/data/icons/${AFTERGLOW_APP_ID}.svg
            DEPENDS ${PROJECT_SOURCE_DIR}/data/icons/${AFTERGLOW_APP_ID}.svg
            COMMENT "Rendering the ${size}px icon")
        list(APPEND _icon_pngs ${_png})
        install(FILES ${_png} DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps)
    endforeach()
    add_custom_target(afterglow_icons ALL DEPENDS ${_icon_pngs})
endif()

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
