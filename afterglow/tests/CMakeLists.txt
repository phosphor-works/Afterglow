# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(GTest REQUIRED)

# Application sources compiled again into a test keep the definitions they
# ship with. The tests themselves build strings from literals.
set_source_files_properties(../src/ProjectStore.cpp ../src/SplitTree.cpp ../src/OverviewNavigation.cpp ../src/DockArea.cpp
    ../src/animation/MomentumScroll.cpp
    ../src/desktop/FileTransfer.cpp ../src/desktop/DragDrop.cpp ../src/desktop/UploadArchive.cpp ../src/desktop/Download.cpp
    PROPERTIES COMPILE_DEFINITIONS QT_NO_CAST_FROM_ASCII)

qt_add_executable(afterglow_app_tests
    ProjectStoreTest.cpp
    ../src/ProjectStore.cpp
    SplitTreeTest.cpp
    ../src/SplitTree.cpp
    OverviewNavigationTest.cpp
    ../src/OverviewNavigation.cpp
    DockAreaTest.cpp
    ../src/DockArea.cpp
    MomentumScrollTest.cpp
    ../src/animation/MomentumScroll.cpp
)
target_include_directories(afterglow_app_tests PRIVATE ../src)
target_link_libraries(afterglow_app_tests PRIVATE Qt6::Core Afterglow::Vt Afterglow::Render GTest::gtest_main)
set_target_properties(afterglow_app_tests PROPERTIES AUTOMOC OFF)

include(GoogleTest)
gtest_discover_tests(afterglow_app_tests)

qt_add_executable(afterglow_scripting_tests ScriptHostTest.cpp ScriptPackageTest.cpp)
target_link_libraries(afterglow_scripting_tests PRIVATE afterglow_scripting GTest::gtest)
target_compile_definitions(afterglow_scripting_tests PRIVATE
    AFTERGLOW_EXAMPLE_PACKAGES="${PROJECT_SOURCE_DIR}/examples/packages"
    AFTERGLOW_EXAMPLE_SCRIPTS="${PROJECT_SOURCE_DIR}/examples/scripts"
    AFTERGLOW_PHOSPHOR_SCRIPTS="${PROJECT_SOURCE_DIR}/examples/phosphor/scripts"
    AFTERGLOW_GLASSHOUSE_SCRIPTS="${PROJECT_SOURCE_DIR}/examples/glasshouse/scripts")
gtest_discover_tests(afterglow_scripting_tests PROPERTIES TIMEOUT 60)

qt_add_executable(afterglow_config_tests ConfigTest.cpp DesktopTest.cpp)
# afterglow_mux_protocol: the bounds of the Mux settings are the daemon's.
target_link_libraries(afterglow_config_tests PRIVATE afterglow_config afterglow_mux_protocol GTest::gtest_main)
target_compile_definitions(afterglow_config_tests PRIVATE AFTERGLOW_THEME_DIR="${PROJECT_SOURCE_DIR}/data/themes"
    AFTERGLOW_BUNDLED_SHADERS="${PROJECT_SOURCE_DIR}/data/shaders")
gtest_discover_tests(afterglow_config_tests)

qt_add_executable(afterglow_upload_tests UploadArchiveTest.cpp ../src/desktop/UploadArchive.cpp ../src/desktop/UploadArchive.h)
target_include_directories(afterglow_upload_tests PRIVATE ../src)
# afterglow_config and AfterglowPty: a test checks the largest
# Terminal.uploadLimit against the input queue of a PTY.
target_link_libraries(afterglow_upload_tests PRIVATE Qt6::Core afterglow_config Afterglow::Proto Afterglow::Pty GTest::gtest_main)
gtest_discover_tests(afterglow_upload_tests PROPERTIES TIMEOUT 60)

# A download's file: its writer thread, its queue and its rename.
qt_add_executable(afterglow_download_tests DownloadFileTest.cpp ../src/desktop/Download.cpp ../src/desktop/Download.h)
target_include_directories(afterglow_download_tests PRIVATE ../src)
target_link_libraries(afterglow_download_tests PRIVATE Qt6::Core GTest::gtest_main)
set_target_properties(afterglow_download_tests PROPERTIES AUTOMOC OFF)
gtest_discover_tests(afterglow_download_tests PROPERTIES TIMEOUT 60)

qt_add_executable(afterglow_control_tests ControlSocketTest.cpp)
target_link_libraries(afterglow_control_tests PRIVATE afterglow_app GTest::gtest)
gtest_discover_tests(afterglow_control_tests PROPERTIES TIMEOUT 60)

qt_add_executable(afterglow_command_tests CommandSocketTest.cpp)
target_link_libraries(afterglow_command_tests PRIVATE afterglow_app Afterglow::Proto GTest::gtest)
# The tests run `aglow run` itself against their own socket, and through
# `afterglow run`, which runs the aglow next to it. Without aglow they skip,
# and one test checks what `afterglow run` says then: it needs the afterglow
# of every build.
afterglow_test_runs(afterglow_command_tests AFTERGLOW_BINARY afterglow)
if(AFTERGLOW_BUILD_AGLOW)
    afterglow_test_runs(afterglow_command_tests AFTERGLOW_AGLOW aglow)
endif()
gtest_discover_tests(afterglow_command_tests PROPERTIES TIMEOUT 60)

# Remote commands: the forwarded sockets, the probe of `afterglow ssh` with a
# fake ssh, and an acceptance test with sshd run as the user, which skips
# without sshd. A pane runs in it, offscreen.
qt_add_executable(afterglow_remote_tests RemoteCommandsTest.cpp)
target_link_libraries(afterglow_remote_tests PRIVATE afterglow_app Afterglow::Proto GTest::gtest)
afterglow_test_runs(afterglow_remote_tests AFTERGLOW_BINARY afterglow)
target_compile_definitions(afterglow_remote_tests PRIVATE AFTERGLOW_FAKE_SSH="${CMAKE_CURRENT_SOURCE_DIR}/fake-ssh.sh")
if(AFTERGLOW_BUILD_AGLOW)
    afterglow_test_runs(afterglow_remote_tests AFTERGLOW_AGLOW aglow)
endif()
gtest_discover_tests(afterglow_remote_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)

# afterglow mux, afterglow ctl on sessions, and afterglow mux --ssh through
# a fake ssh, against daemons of their own, in directories of the test.
# afterglow_config: a test checks that the protocol takes every value of the
# Mux settings (SettingsStore). It attaches to a session as a window does,
# through phosphor-channel.
qt_add_executable(afterglow_mux_command_tests MuxCommandTest.cpp)
target_link_libraries(afterglow_mux_command_tests PRIVATE afterglow_config afterglow_mux_protocol
                      PhosphorChannel::PhosphorChannel GTest::gtest_main)
# afterglow mux runs the aglow-mux next to afterglow.
afterglow_test_runs(afterglow_mux_command_tests AFTERGLOW_BINARY afterglow)
add_dependencies(afterglow_mux_command_tests aglow-mux)
gtest_discover_tests(afterglow_mux_command_tests PROPERTIES TIMEOUT 120)

qt_add_executable(afterglow_pane_tests PaneProtocolTest.cpp PaneSelectionTest.cpp NestedLoopTest.cpp PaneLaunchTest.cpp)
target_link_libraries(afterglow_pane_tests PRIVATE afterglow_app afterglow_image_test_files GTest::gtest)
# The panes decode the images that programs send with the aglow-mux next to
# afterglow (ProgramImages.cpp). These tests set the aglow-mux of this build
# instead, and send it the image files of the tests of the decoders
# (libs/afterglow-image). The decoders have their own tests in
# libs/afterglow-image/tests and mux/tests.
afterglow_test_runs(afterglow_pane_tests AFTERGLOW_MUX aglow-mux)
gtest_discover_tests(afterglow_pane_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

# A window's link to a session (MuxClient), with panes and no window, against
# a daemon of its own (mux/tests/TestDaemon.h).
qt_add_executable(afterglow_mux_client_tests MuxClientTest.cpp)
target_link_libraries(afterglow_mux_client_tests PRIVATE afterglow_app GTest::gtest)
target_include_directories(afterglow_mux_client_tests PRIVATE ${PROJECT_SOURCE_DIR}/mux/tests)
afterglow_test_runs(afterglow_mux_client_tests AFTERGLOW_MUX aglow-mux)
gtest_discover_tests(afterglow_mux_client_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)

# A popup as the window's settings make it look, rendered offscreen.
qt_add_executable(afterglow_popup_tests PopupLookTest.cpp)
# The device of the renderer's tests (GpuTestDevice.h).
target_include_directories(afterglow_popup_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/afterglow-render/tests)
target_link_libraries(afterglow_popup_tests PRIVATE afterglow_app GTest::gtest)
gtest_discover_tests(afterglow_popup_tests PROPERTIES RESOURCE_LOCK gpu LABELS gpu TIMEOUT 60)

# The pointer maps of the bundled and example packs against their pictures, offscreen.
qt_add_executable(afterglow_pointer_map_tests PointerMapTest.cpp)
target_include_directories(afterglow_pointer_map_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/afterglow-render/tests)
# The pack registry and the renderer: no window, no application.
target_link_libraries(afterglow_pointer_map_tests PRIVATE afterglow_shaders GTest::gtest)
target_compile_definitions(afterglow_pointer_map_tests PRIVATE AFTERGLOW_SOURCE_SHADERS="${PROJECT_SOURCE_DIR}/data/shaders"
    AFTERGLOW_EXAMPLE_SHADERS="${PROJECT_SOURCE_DIR}/examples/shaders")
gtest_discover_tests(afterglow_pointer_map_tests PROPERTIES RESOURCE_LOCK gpu LABELS gpu TIMEOUT 60)

qt_add_executable(afterglow_signal_tests QuitSignalsTest.cpp)
target_link_libraries(afterglow_signal_tests PRIVATE afterglow_app GTest::gtest)
gtest_discover_tests(afterglow_signal_tests)

qt_add_executable(afterglow_audio_tests AudioControllerTest.cpp)
target_link_libraries(afterglow_audio_tests PRIVATE afterglow_audio GTest::gtest)
gtest_discover_tests(afterglow_audio_tests)

qt_add_executable(afterglow_shell_tests ShellIntegrationTest.cpp)
target_link_libraries(afterglow_shell_tests PRIVATE afterglow_shell afterglow_mux_protocol Afterglow::Vt GTest::gtest_main)
afterglow_test_runs(afterglow_shell_tests AFTERGLOW_BINARY afterglow)
target_compile_definitions(afterglow_shell_tests PRIVATE AFTERGLOW_TEST_SHELL_DIR="${PROJECT_SOURCE_DIR}/data/shell-integration"
    AFTERGLOW_TEST_TERMINFO_SOURCE="${PROJECT_SOURCE_DIR}/data/terminfo/afterglow.terminfo"
    AFTERGLOW_TEST_TERMINFO_DIR="${CMAKE_BINARY_DIR}/share/terminfo")
gtest_discover_tests(afterglow_shell_tests)

qt_add_executable(afterglow_shader_tests ShaderControlsTest.cpp)
target_link_libraries(afterglow_shader_tests PRIVATE afterglow_shaders GTest::gtest)
target_compile_definitions(afterglow_shader_tests PRIVATE AFTERGLOW_SHADER_DATA_DIR="${PROJECT_SOURCE_DIR}/data"
    AFTERGLOW_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")
gtest_discover_tests(afterglow_shader_tests)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    # The shell completions against a stub afterglow. Exit status 77: no shell to test.
    add_test(NAME afterglow_shell_completion COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/completion_test.py)
    set_tests_properties(afterglow_shell_completion PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    # Fails when docs/CONFIGURATION.md is not what the schema generates.
    add_test(NAME afterglow_configuration_reference
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/generate-reference.py --check $<TARGET_FILE:afterglow>)
    set_tests_properties(afterglow_configuration_reference PROPERTIES TIMEOUT 60)
    # File transfer between the service (afterglow_transfer_tests --protocol-relay) and a client in a
    # pseudo-terminal: kitten when it is installed (77: skipped), and aglow. kitten 0.49.2 leaves out an
    # empty file that it receives, and the service tests check empty files on their own.
    add_test(NAME afterglow_transfer_kitten_relay
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/kitty_transfer_interop.py
                --relay $<TARGET_FILE:afterglow_transfer_tests> --allow-client-empty-file-bug)
    set_tests_properties(afterglow_transfer_kitten_relay PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    if(AFTERGLOW_BUILD_AGLOW)
        add_test(NAME afterglow_transfer_aglow_relay
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/kitty_transfer_interop.py
                    --relay $<TARGET_FILE:afterglow_transfer_tests> --aglow $<TARGET_FILE:aglow>)
        set_tests_properties(afterglow_transfer_aglow_relay PROPERTIES TIMEOUT 300)
    endif()
endif()

option(AFTERGLOW_NATIVE_TESTS "Run integration tests in a nested KWin desktop (requires DISPLAY)" OFF)
if(AFTERGLOW_NATIVE_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME afterglow_native_window_acceptance
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/native_window_acceptance.py
                --binary $<TARGET_FILE:afterglow_native_tests>)
    # The window and Kitty draw on the GPU: they take turns with the renderer's tests.
    set_tests_properties(afterglow_native_window_acceptance PROPERTIES LABELS native TIMEOUT 1820 RESOURCE_LOCK gpu)
    # aglow transfer against Kitty in a headless KWin (77: no kitty, kwin_wayland or dbus-run-session).
    if(AFTERGLOW_BUILD_AGLOW)
        add_test(NAME afterglow_transfer_kitty
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/kitty_transfer_interop.py
                    --kitty --aglow $<TARGET_FILE:aglow>)
        set_tests_properties(afterglow_transfer_kitty PROPERTIES LABELS native SKIP_RETURN_CODE 77 TIMEOUT 660 RESOURCE_LOCK gpu)
    endif()
endif()

qt_add_executable(afterglow_transfer_tests FileTransferTest.cpp ../src/desktop/FileTransfer.cpp ../src/desktop/FileTransfer.h)
target_include_directories(afterglow_transfer_tests PRIVATE ../src)
# zlib and xxHash directly: the tests check the service against them, not
# against the protocol library it uses.
target_link_libraries(afterglow_transfer_tests PRIVATE Qt6::Core Afterglow::Proto afterglow_zlib afterglow_xxhash GTest::gtest)
gtest_discover_tests(afterglow_transfer_tests)
# The TransferLimits tests reach the descriptor limit of transfers, half of
# RLIMIT_NOFILE. They skip unless the soft limit is 64 or less, so in this
# entry a skip, or no test that ran, means that the limit did not apply.
add_test(NAME afterglow_transfer_descriptor_limits
    COMMAND sh -c "ulimit -n 64 && exec \"$0\" --gtest_filter='TransferLimits.*'" $<TARGET_FILE:afterglow_transfer_tests>)
set_tests_properties(afterglow_transfer_descriptor_limits PROPERTIES
    FAIL_REGULAR_EXPRESSION "\\[  SKIPPED \\];\\[  PASSED  \\] 0 tests" TIMEOUT 60)

qt_add_executable(afterglow_dnd_tests DragDropTest.cpp ../src/desktop/DragDrop.cpp ../src/desktop/DragDrop.h)
target_include_directories(afterglow_dnd_tests PRIVATE ../src)
target_link_libraries(afterglow_dnd_tests PRIVATE Qt6::Gui Qt6::GuiPrivate Afterglow::Proto GTest::gtest)
# The tests of a drop on aglow dnd run the aglow of this build.
if(AFTERGLOW_BUILD_AGLOW)
    afterglow_test_runs(afterglow_dnd_tests AFTERGLOW_AGLOW aglow)
endif()
gtest_discover_tests(afterglow_dnd_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Every test executable here names Afterglow's folder before main(), as
# afterglow does (AppDirectory.cpp): PhosphorPaths aborts on a path read before
# the folder has a name, and the tests read their paths as the application does.
add_library(afterglow_test_app_directory OBJECT AppDirectory.cpp)
target_compile_definitions(afterglow_test_app_directory PRIVATE QT_NO_CAST_FROM_ASCII)
target_link_libraries(afterglow_test_app_directory PUBLIC afterglow_paths)
get_property(afterglow_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(test IN LISTS afterglow_test_targets)
    get_target_property(type ${test} TYPE)
    if(type STREQUAL "EXECUTABLE")
        target_link_libraries(${test} PRIVATE afterglow_test_app_directory)
    endif()
endforeach()
