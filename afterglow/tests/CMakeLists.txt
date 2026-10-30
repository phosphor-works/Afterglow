# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(GTest REQUIRED)

qt_add_executable(afterglow_app_tests
    SplitTreeTest.cpp
    ../src/SplitTree.cpp
)
target_include_directories(afterglow_app_tests PRIVATE ../src)
target_link_libraries(afterglow_app_tests PRIVATE Qt6::Core GTest::gtest_main)
set_target_properties(afterglow_app_tests PROPERTIES AUTOMOC OFF)

include(GoogleTest)
gtest_discover_tests(afterglow_app_tests)

qt_add_executable(afterglow_scripting_tests ScriptHostTest.cpp)
target_link_libraries(afterglow_scripting_tests PRIVATE afterglow_scripting GTest::gtest)
target_compile_definitions(afterglow_scripting_tests PRIVATE
    AFTERGLOW_EXAMPLE_SCRIPTS="${PROJECT_SOURCE_DIR}/data/examples/scripts"
    AFTERGLOW_PHOSPHOR_SCRIPTS="${PROJECT_SOURCE_DIR}/data/examples/phosphor/scripts"
    AFTERGLOW_GLASSHOUSE_SCRIPTS="${PROJECT_SOURCE_DIR}/data/examples/glasshouse/scripts")
gtest_discover_tests(afterglow_scripting_tests)

qt_add_executable(afterglow_config_tests ConfigTest.cpp)
target_link_libraries(afterglow_config_tests PRIVATE afterglow_config GTest::gtest_main)
target_compile_definitions(afterglow_config_tests PRIVATE AFTERGLOW_THEME_DIR="${PROJECT_SOURCE_DIR}/data/themes")
gtest_discover_tests(afterglow_config_tests)

qt_add_executable(afterglow_audio_tests AudioControllerTest.cpp)
target_link_libraries(afterglow_audio_tests PRIVATE afterglow_audio GTest::gtest)
gtest_discover_tests(afterglow_audio_tests)
