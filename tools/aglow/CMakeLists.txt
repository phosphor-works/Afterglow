# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# aglow: kitty's file transfer, drag and drop, graphics and clipboard
# protocols from the command line, as one static binary. It uses only
# libs/afterglow-proto (with its bundled zlib-ng and xxHash) and the vendored
# stb_image: no Qt and no Phosphor, so that it builds alone for a remote host.

afterglow_vendored_file(stb_image AGLOW_STB_IMAGE)
get_filename_component(AGLOW_STB_DIR "${AGLOW_STB_IMAGE}" DIRECTORY)

# A sanitizer build (the debug preset) cannot link statically: aglow is then
# a dynamic binary, and the test of a static one is left out.
string(FIND "${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}" "-fsanitize" AGLOW_SANITIZED)
if(AGLOW_SANITIZED EQUAL -1)
    set(AGLOW_STATIC ON)
    # glibc and libstdc++ come as static libraries in their own packages on
    # some distributions. Without them, say what to install, before a long build.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LINK_OPTIONS -static)
    check_cxx_source_compiles("#include <string>\nint main(int argc, char**) { return int(std::string(std::size_t(argc), 'x').size()); }"
                              AGLOW_STATIC_LINK_WORKS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT AGLOW_STATIC_LINK_WORKS)
        message(FATAL_ERROR "aglow links statically, and a static C++ program does not link here. "
                            "Install the static C and C++ runtime libraries (the packages glibc-static and "
                            "libstdc++-static, as Fedora names them), or configure with -DAFTERGLOW_BUILD_AGLOW=OFF.")
    endif()
else()
    set(AGLOW_STATIC OFF)
    message(STATUS "aglow: sanitizers are on, so aglow links dynamically and the aglow_static test is left out")
endif()

# stb_image's MIT notice installs with aglow, as the notices of zlib-ng and
# xxHash do. It is the notice in the vendored header itself.
file(READ "${AGLOW_STB_IMAGE}" AGLOW_STB_TEXT)
string(REGEX MATCH "ALTERNATIVE A - MIT License\n(Copyright[^\n]*\n.*SOFTWARE\.\n)-----" AGLOW_STB_MATCH "${AGLOW_STB_TEXT}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "The MIT notice was not found in ${AGLOW_STB_IMAGE}")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.stb_image" "stb_image ${AFTERGLOW_VENDORED_stb_image_URL}\n\n${CMAKE_MATCH_1}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.stb_image" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow)

# The implementation of stb_image, without Afterglow's warnings: upstream code
# is not ours to fix. SYSTEM keeps its header quiet where aglow includes it.
add_library(aglow_stb_image STATIC StbImage.cpp)
target_include_directories(aglow_stb_image SYSTEM PUBLIC "${AGLOW_STB_DIR}")
target_compile_options(aglow_stb_image PRIVATE -w)
set_target_properties(aglow_stb_image PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF AUTOMOC OFF AUTOUIC OFF)

add_executable(aglow
    main.cpp
    Cli.cpp
    Cli.h
    Clipboard.cpp
    Commands.h
    Dnd.cpp
    Icat.cpp
    Image.cpp
    Image.h
    Transfer.cpp
    Tty.cpp
    Tty.h
)
target_link_libraries(aglow PRIVATE Afterglow::Proto aglow_stb_image)
target_compile_definitions(aglow PRIVATE AGLOW_VERSION="${PROJECT_VERSION}")
# Static: the binary runs on any Linux of its architecture, with no shared
# library to match. It makes no NSS call (getpwnam, getaddrinfo), which glibc
# cannot link statically.
if(AGLOW_STATIC)
    target_link_options(aglow PRIVATE -static)
endif()
set_target_properties(aglow PROPERTIES AUTOMOC OFF AUTOUIC OFF)
# Next to the afterglow executable in the build tree, as after install, so
# that afterglow transfer, dnd, icat and clipboard find it in both.
if(TARGET afterglow)
    set_target_properties(aglow PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/afterglow")
endif()
install(TARGETS aglow RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${PROJECT_SOURCE_DIR}/data/completions/aglow.bash"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions RENAME aglow)
install(FILES "${PROJECT_SOURCE_DIR}/data/completions/_aglow"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions)
install(FILES "${PROJECT_SOURCE_DIR}/data/completions/aglow.fish"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d)

if(BUILD_TESTING)
    # readelf -d of a static binary lists no NEEDED library.
    find_program(AGLOW_READELF NAMES readelf eu-readelf)
    if(AGLOW_READELF AND AGLOW_STATIC)
        add_test(NAME aglow_static
                 COMMAND ${CMAKE_COMMAND} -DREADELF=${AGLOW_READELF} -DBINARY=$<TARGET_FILE:aglow>
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckStatic.cmake")
    elseif(AGLOW_STATIC)
        message(STATUS "aglow: readelf was not found, so the aglow_static test is left out (install binutils to add it)")
    endif()
    add_test(NAME aglow_version COMMAND aglow --version)
    set_tests_properties(aglow_version PROPERTIES PASS_REGULAR_EXPRESSION "^aglow ${PROJECT_VERSION} \\(protocol [0-9]+\\)")
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # aglow clipboard against a terminal that the test plays in a pseudo-terminal.
        add_test(NAME aglow_clipboard_pty
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/clipboard_pty.py $<TARGET_FILE:aglow>)
        set_tests_properties(aglow_clipboard_pty PROPERTIES TIMEOUT 120)
    endif()
    # What icat writes, drawn by the terminal's own renderer (which needs a Vulkan driver).
    if(AFTERGLOW_BUILD_TOOLS)
        if(Python3_Interpreter_FOUND)
            add_test(NAME aglow_icat_render
                     COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/icat_render.py
                             $<TARGET_FILE:aglow> $<TARGET_FILE:afterglow_renderpng>)
        endif()
    endif()
    if(TARGET afterglow)
        add_test(NAME afterglow_runs_aglow COMMAND afterglow icat --help)
        set_tests_properties(afterglow_runs_aglow PROPERTIES PASS_REGULAR_EXPRESSION "Usage: aglow icat")
    endif()
endif()
