# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(GTest REQUIRED)

add_executable(afterglow_vt_tests
    ClipboardTest.cpp
    ConformanceTest.cpp
    GraphicsTest.cpp
    InputTest.cpp
    NotificationTest.cpp
    ParserTest.cpp
    ProgramStatusTest.cpp
    ScreenTest.cpp
    SearchTest.cpp
    SelectionTest.cpp
    TerminalTest.cpp
    UnicodeTest.cpp
)
target_link_libraries(afterglow_vt_tests PRIVATE Afterglow::Vt GTest::gtest_main ZLIB::ZLIB)
set_target_properties(afterglow_vt_tests PROPERTIES AUTOMOC OFF)

include(GoogleTest)
gtest_discover_tests(afterglow_vt_tests)
