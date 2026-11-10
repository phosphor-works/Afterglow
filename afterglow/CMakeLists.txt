# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Qt6 ${AFTERGLOW_QT_MIN} REQUIRED COMPONENTS Core Concurrent Gui Widgets Qml Quick QuickControls2 DBus Network)
find_package(KF6WindowSystem REQUIRED) # KWin blur behind
find_package(Qt6QuickPrivate CONFIG REQUIRED) # native chrome accessibility bridge
find_package(Qt6GuiPrivate CONFIG REQUIRED) # rhi/qrhi.h for the chrome render target
find_package(Qt6CorePrivate CONFIG REQUIRED) # QMetaObjectBuilder: the schema entries of script commands
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_library(afterglow_app STATIC
    src/desktop/Accessibility.cpp
    src/DockArea.cpp
    src/DockArea.h
    src/NestedLoop.h
    src/Pane.cpp
    src/mux/MuxClient.cpp
    src/mux/MuxClient.h
    src/PaneDialog.cpp
    src/PaneDialog.h
    src/PaneLaunch.cpp
    src/PaneLaunch.h
    src/PaneText.cpp
    src/Pane.h
    src/PaneOverlay.cpp
    src/PaneOverlay.h
    src/ProjectStore.cpp
    src/ProjectStore.h
    src/ShownText.h
    src/SplitTree.cpp
    src/SplitTree.h
    src/TerminalWindowProject.cpp
    src/TerminalWindowLayout.cpp
    src/TerminalWindowOverview.cpp
    src/TerminalWindowScript.cpp
    src/TerminalWindowOverlay.cpp
    src/TerminalWindowDock.cpp
    src/TerminalWindow.cpp
    src/TerminalWindow.h
    src/KeyMapping.cpp
    src/KeyMapping.h
    src/chrome/ChromeController.cpp
    src/chrome/ChromeController.h
    src/chrome/ChromeLayer.cpp
    src/chrome/ChromeLayer.h
    src/animation/FrameClock.cpp
    src/animation/FrameClock.h
    src/animation/MomentumScroll.cpp
    src/animation/MomentumScroll.h
    src/animation/PaneMotion.h
    src/app/Application.cpp
    src/app/InstanceSocket.cpp
    src/app/InstanceSocket.h
    src/app/QuitSignals.cpp
    src/app/QuitSignals.h
    src/app/NotificationCenter.cpp
    src/desktop/FileTransfer.cpp
    src/desktop/FileTransfer.h
    src/desktop/DragDrop.cpp
    src/desktop/DragDrop.h
    src/desktop/ProtoBytes.h
    src/control/Capture.h
    src/control/ControlProject.cpp
    src/control/Control.cpp
    src/control/Control.h
    src/control/CommandSocket.cpp
    src/control/CommandSocket.h
    src/control/ControlSocket.cpp
    src/control/ControlSocket.h
    src/control/RemoteSockets.cpp
    src/control/RemoteSockets.h
    # Here, not with Ctl: the socket tests drive it too.
    src/control/IpcClient.cpp
    src/control/IpcClient.h
    src/scripting/ScriptControl.cpp
    src/scripting/ScriptControl.h
    src/app/NotificationCenter.h
    src/app/Application.h
    src/app/Services.h
)

qt_add_executable(afterglow src/main.cpp src/control/Ctl.cpp src/control/Ctl.h src/control/Mux.cpp src/control/Mux.h
    src/shell/Aglow.cpp src/shell/Aglow.h)
# afterglow_app brings afterglow_mux_protocol, which Mux.cpp uses too.
target_link_libraries(afterglow PRIVATE afterglow_app)
# afterglow mux runs the afterglow-mux of its own directory.
add_dependencies(afterglow afterglow-mux)
# Without aglow, afterglow run, transfer, dnd, icat and clipboard say that it is not built.
if(AFTERGLOW_BUILD_AGLOW)
    target_compile_definitions(afterglow PRIVATE AFTERGLOW_HAS_AGLOW)
endif()

# Shader pack loading, shared with the developer render tool.
# A binary under the build directory reads the packs and themes of the source
# tree; an installed one reads those of its own prefix.
qt_add_library(afterglow_shaders STATIC src/shaders/ShaderLibrary.cpp src/shaders/ShaderLibrary.h
    src/shaders/ShaderControls.cpp src/shaders/ShaderControls.h src/shaders/ShaderRefs.h)
target_include_directories(afterglow_shaders PUBLIC src)
target_compile_definitions(afterglow_shaders PRIVATE QT_NO_CAST_FROM_ASCII
    AFTERGLOW_BUILD_DIR="${CMAKE_BINARY_DIR}"
    AFTERGLOW_SOURCE_SHADER_DIR="${PROJECT_SOURCE_DIR}/data/shaders"
    AFTERGLOW_INSTALLED_SHADER_DIR="${CMAKE_INSTALL_FULL_DATADIR}/afterglow/shaders")
target_link_libraries(afterglow_shaders PUBLIC Afterglow::Render PhosphorShaders::PhosphorShaders PhosphorAnimation::PhosphorAnimation
    PhosphorFsLoader::PhosphorFsLoader Qt6::Core)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

# Settings, keybinds, key modes, themes and the terminal text capture, shared
# with their tests.
qt_add_library(afterglow_config STATIC
    src/desktop/TerminalText.cpp
    src/desktop/TerminalText.h
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
    AFTERGLOW_BUILD_DIR="${CMAKE_BINARY_DIR}"
    AFTERGLOW_SOURCE_THEME_DIR="${PROJECT_SOURCE_DIR}/data/themes"
    AFTERGLOW_INSTALLED_THEME_DIR="${CMAKE_INSTALL_FULL_DATADIR}/afterglow/themes")
target_link_libraries(afterglow_config PUBLIC afterglow_shaders Afterglow::Render Afterglow::Vt
    PhosphorConfig::PhosphorConfig PhosphorFsLoader::PhosphorFsLoader Qt6::Core Qt6::Gui PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON)

qt_add_library(afterglow_shell STATIC src/shell/ShellIntegration.cpp src/shell/ShellIntegration.h
    src/shell/Ssh.cpp src/shell/Ssh.h)
target_include_directories(afterglow_shell PUBLIC src)
target_link_libraries(afterglow_shell PUBLIC Afterglow::Pty Qt6::Core PRIVATE Afterglow::Proto)
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

# An overlay's options and a popup's place: the script host and the control
# socket read them, and afterglow_renderpng places its popups by them.
qt_add_library(afterglow_overlayspec STATIC src/scripting/OverlaySpec.cpp src/scripting/OverlaySpec.h)
target_include_directories(afterglow_overlayspec PUBLIC src)
target_compile_definitions(afterglow_overlayspec PRIVATE QT_NO_CAST_FROM_ASCII)
target_link_libraries(afterglow_overlayspec PUBLIC Qt6::Core)

# The bounded decoder of the images that programs send: the panes decode
# with it, and afterglow_renderpng decodes as they do.
qt_add_library(afterglow_imagedecode STATIC src/ImageDecode.cpp src/ImageDecode.h)
target_include_directories(afterglow_imagedecode PUBLIC src)
target_compile_definitions(afterglow_imagedecode PRIVATE QT_NO_CAST_FROM_ASCII)
# Its header holds only standard types: the libraries it decodes with stay inside.
target_link_libraries(afterglow_imagedecode PUBLIC Qt6::Gui PRIVATE Afterglow::Vt Afterglow::Proto)

# The script host, shared with its tests. The Control binding lives in the app.
qt_add_library(afterglow_scripting STATIC src/scripting/ScriptHost.cpp src/scripting/ScriptHost.h
    src/scripting/ScriptTasks.cpp src/scripting/ScriptTasks.h
    src/scripting/ScriptPackage.cpp src/scripting/ScriptPackage.h
    src/scripting/ScriptState.cpp src/scripting/ScriptState.h)
target_include_directories(afterglow_scripting PUBLIC src)
target_compile_definitions(afterglow_scripting PRIVATE QT_NO_CAST_FROM_ASCII)
target_link_libraries(afterglow_scripting PUBLIC afterglow_overlayspec PhosphorScripting::PhosphorScripting PhosphorFsLoader::PhosphorFsLoader
    Qt6::Core Qt6::Concurrent)
qt_add_resources(afterglow_scripting afterglow_scripting_prelude
    PREFIX /afterglow/scripting
    BASE src/scripting
    FILES src/scripting/afterglow.luau)

function(afterglow_add_chrome target)
qt_add_qml_module(${target}
    URI Afterglow.Chrome
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${target}-qml/Afterglow/Chrome"
    QML_FILES
        qml/Chrome.qml
        qml/ClipboardPrompt.qml
        qml/CommandPalette.qml
        qml/ConfirmPrompt.qml
        qml/ContextMenu.qml
        qml/PasteConfirm.qml
        qml/ScriptDialog.qml
        qml/Scrim.qml
        qml/SearchBar.qml
        qml/StatusIndicator.qml
        qml/TitlePrompt.qml
        qml/TabBar.qml
)
endfunction()
afterglow_add_chrome(afterglow)

if(AFTERGLOW_BUILD_TOOLS)
    qt_add_executable(afterglow_native_bench ../tools/bench/nativebench.cpp)
    target_link_libraries(afterglow_native_bench PRIVATE afterglow_app)
    afterglow_add_chrome(afterglow_native_bench)
endif()

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    qt_add_executable(afterglow_native_tests tests/NativeWindowTest.cpp)
    target_link_libraries(afterglow_native_tests PRIVATE afterglow_app GTest::gtest)
    # The mux tests run a daemon of their own (mux/tests/TestDaemon.h).
    target_include_directories(afterglow_native_tests PRIVATE ${PROJECT_SOURCE_DIR}/mux/tests)
    target_compile_definitions(afterglow_native_tests PRIVATE AFTERGLOW_MUX="$<TARGET_FILE:afterglow-mux>")
    add_dependencies(afterglow_native_tests afterglow-mux)
    # The aglow dnd twins of the kitten dnd tests run the aglow of this build.
    if(AFTERGLOW_BUILD_AGLOW)
        target_compile_definitions(afterglow_native_tests PRIVATE AFTERGLOW_AGLOW="$<TARGET_FILE:aglow>")
        add_dependencies(afterglow_native_tests aglow)
    endif()
    afterglow_add_chrome(afterglow_native_tests)
endif()

# The app id matches QGuiApplication::desktopFileName, which Wayland uses to
# find the icon and group windows, and names the desktop files below.
set(AFTERGLOW_APP_ID com.phosphorworks.Afterglow)
target_compile_definitions(afterglow_app PUBLIC QT_NO_CAST_FROM_ASCII AFTERGLOW_VERSION="${PROJECT_VERSION}"
    AFTERGLOW_APP_ID="${AFTERGLOW_APP_ID}"
    AFTERGLOW_BUILD_DIR="${CMAKE_BINARY_DIR}"
    AFTERGLOW_BUILD_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins"
    AFTERGLOW_INSTALLED_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/afterglow/plugins"
    AFTERGLOW_BUILD_TERMINFO_DIR="${CMAKE_BINARY_DIR}/share/terminfo"
    AFTERGLOW_INSTALLED_TERMINFO_DIR="${CMAKE_INSTALL_FULL_DATADIR}/terminfo")
# The layer-shell QPA plugin is part of the phosphor build; make sure it exists.
add_dependencies(afterglow phosphorwayland-qpa)
target_include_directories(afterglow_app PUBLIC src)
target_link_libraries(afterglow_app PUBLIC afterglow_shell afterglow_config afterglow_shaders afterglow_scripting afterglow_audio afterglow_imagedecode Afterglow::Render Afterglow::Pty Afterglow::Vt Qt6::Core Qt6::Gui
    Qt6::Concurrent Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::CorePrivate Qt6::GuiPrivate Qt6::QuickPrivate
    PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON PhosphorConfig::PhosphorConfig PhosphorShaders::PhosphorShaders
    PhosphorDBus::PhosphorDBus PhosphorIpc::PhosphorIpc PhosphorScripting::PhosphorScripting
    PhosphorAnimation::PhosphorAnimation PhosphorShortcuts::PhosphorShortcuts PhosphorWayland::PhosphorWayland
    Qt6::DBus Qt6::Network KF6::WindowSystem
    # Control.h, CommandSocket.h and RemoteSockets.h include its headers.
    Afterglow::Proto
    # mux/MuxClient.h, which a window and its tests include: a pane can show a
    # pane of a mux session.
    afterglow_mux_protocol)

install(TARGETS afterglow RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Private libraries: afterglow's own and the phosphor ones it was built with.
# They find each other in their own directory, and the plugin finds them two
# levels up.
set(AFTERGLOW_PRIVATE_LIBS AfterglowVt AfterglowRender AfterglowPty
    PhosphorConfig PhosphorFsLoader PhosphorRegistry PhosphorShaders PhosphorRendering
    PhosphorWayland PhosphorLayer PhosphorAnimation PhosphorShortcuts
    PhosphorDBus PhosphorIpc PhosphorScripting PhosphorAudio PhosphorChannel)
set_target_properties(${AFTERGLOW_PRIVATE_LIBS} PROPERTIES INSTALL_RPATH "$ORIGIN")
set_target_properties(phosphorwayland-qpa PROPERTIES INSTALL_RPATH "$ORIGIN/../..")
install(TARGETS ${AFTERGLOW_PRIVATE_LIBS} LIBRARY DESTINATION ${AFTERGLOW_PRIVATE_LIBDIR} NAMELINK_SKIP)
install(TARGETS phosphorwayland-qpa
        LIBRARY DESTINATION ${AFTERGLOW_PRIVATE_LIBDIR}/plugins/wayland-shell-integration)
# Licence texts of what is installed: the code (GPL and LGPL), the themes
# (MIT), the icon (CC-BY-SA-4.0) and the desktop files (CC0-1.0).
set(AFTERGLOW_LICENSE_TEXTS GPL-3.0-or-later LGPL-2.1-or-later MIT CC-BY-SA-4.0 CC0-1.0)
list(TRANSFORM AFTERGLOW_LICENSE_TEXTS PREPEND "${PROJECT_SOURCE_DIR}/LICENSES/")
list(TRANSFORM AFTERGLOW_LICENSE_TEXTS APPEND ".txt")
install(FILES ${AFTERGLOW_LICENSE_TEXTS} DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow)
# zlib-ng and xxHash are linked into the binaries (libs/afterglow-proto), and
# their notices go with them. So do the notices of Luau, which is statically
# linked into PhosphorScripting, and of valijson; the phosphor rules that
# install them are skipped with the others (cmake/AfterglowPhosphor.cmake).
install(FILES "${AFTERGLOW_ZLIB_LICENSE}" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow RENAME LICENSE.zlib-ng)
install(FILES "${AFTERGLOW_XXHASH_LICENSE}" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow
        RENAME LICENSE.xxHash)
# A system Luau (PLASMAZONES_SYSTEM_LUAU) is a separate package with its own
# notices, and phosphor-scripting fetches no archive for it.
if(NOT PLASMAZONES_SYSTEM_LUAU)
    # phosphor_fetch_dependency(luau) sets luau_SOURCE_DIR in that library's scope.
    get_directory_property(AFTERGLOW_LUAU_SOURCE_DIR DIRECTORY "${AFTERGLOW_PHOSPHOR_LIBS_DIR}/libs/phosphor-scripting"
        DEFINITION luau_SOURCE_DIR)
    if(NOT EXISTS "${AFTERGLOW_LUAU_SOURCE_DIR}/LICENSE.txt" OR NOT EXISTS "${AFTERGLOW_LUAU_SOURCE_DIR}/lua_LICENSE.txt")
        message(FATAL_ERROR "The Luau and Lua licenses were not found in '${AFTERGLOW_LUAU_SOURCE_DIR}', and they must install with the binaries.")
    endif()
    install(FILES "${AFTERGLOW_LUAU_SOURCE_DIR}/LICENSE.txt" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow
            RENAME LICENSE.Luau)
    # Luau derives from Lua, whose MIT notice it carries beside its own.
    install(FILES "${AFTERGLOW_LUAU_SOURCE_DIR}/lua_LICENSE.txt" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow
            RENAME LICENSE.Lua)
endif()
# valijson (BSD-2-Clause) is header-only, and its headers are compiled into
# PhosphorFsLoader, so its notice goes with that library. System headers
# (PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON) are a separate package with their own
# notice, and phosphor-fsloader fetches no archive for them.
if(NOT PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON)
    # phosphor_fetch_dependency(valijson) sets valijson_SOURCE_DIR in that library's scope.
    get_directory_property(AFTERGLOW_VALIJSON_SOURCE_DIR DIRECTORY "${AFTERGLOW_PHOSPHOR_LIBS_DIR}/libs/phosphor-fsloader"
        DEFINITION valijson_SOURCE_DIR)
    if(NOT EXISTS "${AFTERGLOW_VALIJSON_SOURCE_DIR}/LICENSE")
        message(FATAL_ERROR "The valijson license was not found in '${AFTERGLOW_VALIJSON_SOURCE_DIR}', and it must install with the binaries.")
    endif()
    install(FILES "${AFTERGLOW_VALIJSON_SOURCE_DIR}/LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow
            RENAME LICENSE.valijson)
endif()
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/shaders/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/shaders)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/themes/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/themes)
# Scripting: types for editors and examples to copy; neither loads on its own.
install(FILES src/scripting/afterglow.d.luau DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/api)
# Shell completion, where each shell looks for the completions of packages.
install(FILES ${PROJECT_SOURCE_DIR}/data/completions/afterglow.bash
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions RENAME afterglow)
# Zsh: the directory comes from AFTERGLOW_ZSH_COMPLETION_DIR (CMakeLists.txt).
install(FILES ${PROJECT_SOURCE_DIR}/data/completions/_afterglow DESTINATION ${AFTERGLOW_ZSH_COMPLETION_INSTALL_DIR})
install(FILES ${PROJECT_SOURCE_DIR}/data/completions/afterglow.fish DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/examples/packages/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/examples/packages)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/examples/projects/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/examples/projects)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/examples/scripts/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/examples/scripts)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/examples/glasshouse/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/examples/glasshouse
        USE_SOURCE_PERMISSIONS)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/examples/phosphor/ DESTINATION ${CMAKE_INSTALL_DATADIR}/afterglow/examples/phosphor
        USE_SOURCE_PERMISSIONS)

# Desktop integration.
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
