# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(GTest REQUIRED)

add_executable(afterglow_proto_tests
    Base64Test.cpp
    ClientTest.cpp
    ClipboardTest.cpp
    ColorTest.cpp
    DeltaTest.cpp
    DragDropTest.cpp
    DndUriTest.cpp
    GraphicsTest.cpp
    HelpersTest.cpp
    IpcServerTest.cpp
    IpcTest.cpp
    JsonTest.cpp
    RemoteTest.cpp
    RunTest.cpp
    SocketTest.cpp
    TextTest.cpp
    TransferTest.cpp
)
# xxHash directly: the delta tests check the library against it.
target_link_libraries(afterglow_proto_tests PRIVATE Afterglow::Proto afterglow_xxhash GTest::gtest_main)
set_target_properties(afterglow_proto_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)

include(GoogleTest)
# Suites such as Clipboard and Graphics have the same names in the
# afterglow-vt tests: the prefix keeps the ctest names apart. The timeout
# ends a test that waits on a socket that never answers.
gtest_discover_tests(afterglow_proto_tests TEST_PREFIX "Proto." PROPERTIES TIMEOUT 60)
