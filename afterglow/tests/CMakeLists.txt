# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(GTest REQUIRED)

# Application sources compiled again into a test keep the definitions they
# ship with. The tests themselves build strings from literals.
set_source_files_properties(../src/ProjectStore.cpp ../src/SplitTree.cpp ../src/DockArea.cpp
    ../src/desktop/FileTransfer.cpp ../src/desktop/DragDrop.cpp PROPERTIES COMPILE_DEFINITIONS QT_NO_CAST_FROM_ASCII)

qt_add_executable(afterglow_app_tests
    ProjectStoreTest.cpp
    ../src/ProjectStore.cpp
    SplitTreeTest.cpp
    ../src/SplitTree.cpp
    DockAreaTest.cpp
    ../src/DockArea.cpp
)
target_include_directories(afterglow_app_tests PRIVATE ../src)
target_link_libraries(afterglow_app_tests PRIVATE Qt6::Core Afterglow::Vt Afterglow::Render GTest::gtest_main)
set_target_properties(afterglow_app_tests PROPERTIES AUTOMOC OFF)

include(GoogleTest)
gtest_discover_tests(afterglow_app_tests)

qt_add_executable(afterglow_scripting_tests ScriptHostTest.cpp ScriptPackageTest.cpp)
target_link_libraries(afterglow_scripting_tests PRIVATE afterglow_scripting GTest::gtest)
target_compile_definitions(afterglow_scripting_tests PRIVATE
    AFTERGLOW_EXAMPLE_PACKAGES="${PROJECT_SOURCE_DIR}/data/examples/packages"
    AFTERGLOW_EXAMPLE_SCRIPTS="${PROJECT_SOURCE_DIR}/data/examples/scripts"
    AFTERGLOW_PHOSPHOR_SCRIPTS="${PROJECT_SOURCE_DIR}/data/examples/phosphor/scripts"
    AFTERGLOW_GLASSHOUSE_SCRIPTS="${PROJECT_SOURCE_DIR}/data/examples/glasshouse/scripts")
gtest_discover_tests(afterglow_scripting_tests)

qt_add_executable(afterglow_config_tests ConfigTest.cpp DesktopTest.cpp)
target_link_libraries(afterglow_config_tests PRIVATE afterglow_config GTest::gtest_main)
target_compile_definitions(afterglow_config_tests PRIVATE AFTERGLOW_THEME_DIR="${PROJECT_SOURCE_DIR}/data/themes")
gtest_discover_tests(afterglow_config_tests)

qt_add_executable(afterglow_control_tests ControlSocketTest.cpp)
target_link_libraries(afterglow_control_tests PRIVATE afterglow_app GTest::gtest)
gtest_discover_tests(afterglow_control_tests)

qt_add_executable(afterglow_command_tests CommandSocketTest.cpp)
target_link_libraries(afterglow_command_tests PRIVATE afterglow_app Afterglow::Proto GTest::gtest)
# The tests run `afterglow run` itself against their own socket.
target_compile_definitions(afterglow_command_tests PRIVATE AFTERGLOW_BINARY="$<TARGET_FILE:afterglow>")
add_dependencies(afterglow_command_tests afterglow)
gtest_discover_tests(afterglow_command_tests)

qt_add_executable(afterglow_pane_tests PaneProtocolTest.cpp)
target_link_libraries(afterglow_pane_tests PRIVATE afterglow_app GTest::gtest)
gtest_discover_tests(afterglow_pane_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(afterglow_signal_tests QuitSignalsTest.cpp)
target_link_libraries(afterglow_signal_tests PRIVATE afterglow_app GTest::gtest)
gtest_discover_tests(afterglow_signal_tests)

qt_add_executable(afterglow_audio_tests AudioControllerTest.cpp)
target_link_libraries(afterglow_audio_tests PRIVATE afterglow_audio GTest::gtest)
gtest_discover_tests(afterglow_audio_tests)

qt_add_executable(afterglow_shell_tests ShellIntegrationTest.cpp)
target_link_libraries(afterglow_shell_tests PRIVATE afterglow_shell Afterglow::Vt GTest::gtest_main)
target_compile_definitions(afterglow_shell_tests PRIVATE AFTERGLOW_TEST_SHELL_DIR="${PROJECT_SOURCE_DIR}/data/shell-integration"
    AFTERGLOW_TEST_TERMINFO_SOURCE="${PROJECT_SOURCE_DIR}/data/terminfo/afterglow.terminfo")
gtest_discover_tests(afterglow_shell_tests)

qt_add_executable(afterglow_shader_tests ShaderControlsTest.cpp)
target_link_libraries(afterglow_shader_tests PRIVATE afterglow_shaders GTest::gtest)
target_compile_definitions(afterglow_shader_tests PRIVATE AFTERGLOW_SHADER_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
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
endif()

option(AFTERGLOW_NATIVE_TESTS "Run integration tests in a nested KWin desktop (requires DISPLAY)" OFF)
if(AFTERGLOW_NATIVE_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME afterglow_native_window_acceptance
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/native_window_acceptance.py
                --binary $<TARGET_FILE:afterglow_native_tests>)
    set_tests_properties(afterglow_native_window_acceptance PROPERTIES LABELS native TIMEOUT 420)
endif()

qt_add_executable(afterglow_transfer_tests FileTransferTest.cpp ../src/desktop/FileTransfer.cpp ../src/desktop/FileTransfer.h)
target_include_directories(afterglow_transfer_tests PRIVATE ../src)
# zlib and xxHash directly: the tests check the service against them, not
# against the protocol library it uses.
target_link_libraries(afterglow_transfer_tests PRIVATE Qt6::Core Afterglow::Proto afterglow_zlib afterglow_xxhash GTest::gtest)
gtest_discover_tests(afterglow_transfer_tests)

qt_add_executable(afterglow_dnd_tests DragDropTest.cpp ../src/desktop/DragDrop.cpp ../src/desktop/DragDrop.h)
target_include_directories(afterglow_dnd_tests PRIVATE ../src)
target_link_libraries(afterglow_dnd_tests PRIVATE Qt6::Gui Afterglow::Proto GTest::gtest)
# A test drops a tree on the aglow of this build.
if(AFTERGLOW_BUILD_AGLOW)
    target_compile_definitions(afterglow_dnd_tests PRIVATE AFTERGLOW_AGLOW="$<TARGET_FILE:aglow>")
    add_dependencies(afterglow_dnd_tests aglow)
endif()
gtest_discover_tests(afterglow_dnd_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
