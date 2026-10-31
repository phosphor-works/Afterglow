# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(GTest REQUIRED)
target_link_libraries(afterglow_native_tests PRIVATE GTest::gtest)

qt_add_executable(afterglow_app_tests
    ProjectStoreTest.cpp
    ../src/ProjectStore.cpp
    SplitTreeTest.cpp
    ../src/SplitTree.cpp
)
target_include_directories(afterglow_app_tests PRIVATE ../src)
target_link_libraries(afterglow_app_tests PRIVATE Qt6::Core GTest::gtest_main)
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

qt_add_executable(afterglow_audio_tests AudioControllerTest.cpp)
target_link_libraries(afterglow_audio_tests PRIVATE afterglow_audio GTest::gtest)
gtest_discover_tests(afterglow_audio_tests)

qt_add_executable(afterglow_shell_tests ShellIntegrationTest.cpp)
target_link_libraries(afterglow_shell_tests PRIVATE afterglow_shell Afterglow::Vt GTest::gtest_main)
target_compile_definitions(afterglow_shell_tests PRIVATE AFTERGLOW_TEST_SHELL_DIR="${PROJECT_SOURCE_DIR}/data/shell-integration")
gtest_discover_tests(afterglow_shell_tests)

qt_add_executable(afterglow_shader_tests ShaderControlsTest.cpp)
target_link_libraries(afterglow_shader_tests PRIVATE afterglow_shaders GTest::gtest)
target_compile_definitions(afterglow_shader_tests PRIVATE AFTERGLOW_SHADER_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
gtest_discover_tests(afterglow_shader_tests)

option(AFTERGLOW_NATIVE_TESTS "Run integration tests in a nested KWin desktop (requires DISPLAY)" OFF)
if(AFTERGLOW_NATIVE_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME afterglow_native_window_acceptance
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/native_window_acceptance.py
                --binary $<TARGET_FILE:afterglow_native_tests>)
    set_tests_properties(afterglow_native_window_acceptance PROPERTIES LABELS native TIMEOUT 120)
endif()

qt_add_executable(afterglow_transfer_tests FileTransferTest.cpp ../src/desktop/FileTransfer.cpp ../src/desktop/FileTransfer.h)
target_include_directories(afterglow_transfer_tests PRIVATE ../src)
target_link_libraries(afterglow_transfer_tests PRIVATE Qt6::Core ZLIB::ZLIB PkgConfig::XXHASH GTest::gtest)
gtest_discover_tests(afterglow_transfer_tests)

qt_add_executable(afterglow_dnd_tests DragDropTest.cpp ../src/desktop/DragDrop.cpp ../src/desktop/DragDrop.h)
target_include_directories(afterglow_dnd_tests PRIVATE ../src)
target_link_libraries(afterglow_dnd_tests PRIVATE Qt6::Gui GTest::gtest)
gtest_discover_tests(afterglow_dnd_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
