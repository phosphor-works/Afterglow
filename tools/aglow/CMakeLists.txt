# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# aglow, as one static binary: the Kitty file transfer, drag and drop,
# graphics and clipboard protocols from the command line, and the client of
# the commands of Afterglow's scripts (aglow run). It uses only
# libs/afterglow-proto (with its bundled zlib-ng and xxHash) and the vendored
# stb_image: no Qt and no Phosphor, so that the protocols run alone on a
# remote host. aglow run talks to an Afterglow on the same host.

afterglow_vendored_file(stb_image AGLOW_STB_IMAGE)
get_filename_component(AGLOW_STB_DIR "${AGLOW_STB_IMAGE}" DIRECTORY)

# Writes a file only when its text changes, so that a configure does not make
# the install copy it again.
function(aglow_write_file path content)
    if(EXISTS "${path}")
        file(READ "${path}" old)
        if(old STREQUAL content)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()

# A sanitizer build (the debug preset) cannot link statically: aglow is then
# a dynamic binary, and the test of a static one is left out. The flags of
# each build type count too, as the compiler gets them.
set(AGLOW_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
foreach(config IN LISTS CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" config)
    string(APPEND AGLOW_FLAGS " ${CMAKE_CXX_FLAGS_${config}} ${CMAKE_C_FLAGS_${config}} ${CMAKE_EXE_LINKER_FLAGS_${config}}")
endforeach()
string(FIND "${AGLOW_FLAGS}" "-fsanitize" AGLOW_SANITIZED)
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
        # The cached result would fail the next configure too, after the
        # libraries are installed.
        unset(AGLOW_STATIC_LINK_WORKS CACHE)
        message(FATAL_ERROR "aglow links statically, and a static C++ program does not link here. "
                            "Install the static C and C++ runtime libraries (the packages glibc-static and "
                            "libstdc++-static, as Fedora names them), or configure with -DAFTERGLOW_BUILD_AGLOW=OFF. "
                            "That build has no aglow, and so no afterglow run, transfer, dnd, icat or clipboard.")
    endif()
else()
    set(AGLOW_STATIC OFF)
    message(STATUS "aglow: sanitizers are on, so aglow links dynamically and the aglow_static test is left out")
endif()

# stb_image's MIT notice installs with aglow, as the notices of zlib-ng and
# xxHash do. It is the notice in the vendored header itself, from the MIT
# alternative up to the next one. A regular expression cannot cut it there:
# CMake has no lazy match, and ".*" runs on to the end of the Unlicense.
file(READ "${AGLOW_STB_IMAGE}" AGLOW_STB_TEXT)
string(FIND "${AGLOW_STB_TEXT}" "ALTERNATIVE A - MIT License\n" AGLOW_STB_MIT)
string(FIND "${AGLOW_STB_TEXT}" "ALTERNATIVE B - " AGLOW_STB_UNLICENSE)
if(AGLOW_STB_MIT EQUAL -1 OR AGLOW_STB_UNLICENSE LESS_EQUAL AGLOW_STB_MIT)
    message(FATAL_ERROR "The MIT notice was not found in ${AGLOW_STB_IMAGE}")
endif()
math(EXPR AGLOW_STB_LENGTH "${AGLOW_STB_UNLICENSE} - ${AGLOW_STB_MIT}")
string(SUBSTRING "${AGLOW_STB_TEXT}" ${AGLOW_STB_MIT} ${AGLOW_STB_LENGTH} AGLOW_STB_MIT_TEXT)
# Without the heading line and the rule of dashes that ends the alternative.
string(REGEX REPLACE "^ALTERNATIVE A - MIT License\n" "" AGLOW_STB_MIT_TEXT "${AGLOW_STB_MIT_TEXT}")
string(REGEX REPLACE "-+\n$" "" AGLOW_STB_MIT_TEXT "${AGLOW_STB_MIT_TEXT}")
if(NOT AGLOW_STB_MIT_TEXT MATCHES "^Copyright[^\n]*\n.*SOFTWARE\\.\n$")
    message(FATAL_ERROR "The MIT notice in ${AGLOW_STB_IMAGE} does not have the expected form")
endif()
aglow_write_file("${CMAKE_CURRENT_BINARY_DIR}/LICENSE.stb_image"
                 "stb_image ${AFTERGLOW_VENDORED_stb_image_URL}, with the patches of aglow\n\n${AGLOW_STB_MIT_TEXT}")
set(AGLOW_NOTICES "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.stb_image")
# A static aglow holds parts of glibc (LGPL-2.1-or-later) and of libstdc++
# and libgcc (GPL-3.0-or-later with the GCC Runtime Library Exception). Their
# notices go with it. The glibc one says how to link aglow again with another
# C library, as section 6 of the LGPL asks. A dynamic aglow uses the system's
# libraries, which come with their own notices.
if(AGLOW_STATIC)
    set(AGLOW_GCC_EXCEPTION "${CMAKE_CURRENT_SOURCE_DIR}/notices/COPYING.RUNTIME")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/notices/libstdc++.txt" AGLOW_LIBSTDCXX_TEXT)
    file(READ "${AGLOW_GCC_EXCEPTION}" AGLOW_GCC_EXCEPTION_TEXT)
    aglow_write_file("${CMAKE_CURRENT_BINARY_DIR}/LICENSE.libstdc++" "${AGLOW_LIBSTDCXX_TEXT}${AGLOW_GCC_EXCEPTION_TEXT}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/notices/libstdc++.txt" "${AGLOW_GCC_EXCEPTION}")
    list(APPEND AGLOW_NOTICES "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.libstdc++")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/notices/glibc.txt" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow
            RENAME LICENSE.glibc)
endif()
install(FILES ${AGLOW_NOTICES} DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/afterglow)

# The implementation of stb_image, without Afterglow's warnings: upstream code
# is not ours to fix. SYSTEM keeps its header quiet where aglow includes it.
add_library(aglow_stb_image STATIC StbImage.cpp)
target_include_directories(aglow_stb_image SYSTEM PUBLIC "${AGLOW_STB_DIR}")
# -fwrapv: the scalar IDCT and dequantization of stb overflow signed ints on
# hostile input; wrapping is defined, not undefined.
target_compile_options(aglow_stb_image PRIVATE -w -fwrapv)
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
    Mime.h
    Run.cpp
    StbImage.h
    Transfer.cpp
    Tty.cpp
    Tty.h
)
target_link_libraries(aglow PRIVATE Afterglow::Proto aglow_stb_image)
target_compile_definitions(aglow PRIVATE AGLOW_VERSION="${PROJECT_VERSION}")
# shm_open (icat's shared memory) moved from librt into libc in glibc 2.34.
include(CheckSymbolExists)
check_symbol_exists(shm_open "sys/mman.h" AGLOW_SHM_OPEN_IN_LIBC)
if(NOT AGLOW_SHM_OPEN_IN_LIBC)
    target_link_libraries(aglow PRIVATE rt)
endif()
# Static: the binary needs no shared library to match on the host that runs
# it. It still needs the kernel and the processor level that the static C
# library of the build host asks for: the build prints both. It makes no NSS
# call (getpwnam, getaddrinfo), which glibc cannot link statically.
if(AGLOW_STATIC)
    target_link_options(aglow PRIVATE -static)
    if(CMAKE_READELF)
        add_custom_command(TARGET aglow POST_BUILD
                           COMMAND ${CMAKE_COMMAND} -DREADELF=${CMAKE_READELF} -DBINARY=$<TARGET_FILE:aglow> -DNOTES_ONLY=ON
                                   -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckStatic.cmake"
                           VERBATIM)
    endif()
endif()
# Next to the afterglow executable in the build tree, as after install, so
# that afterglow run, transfer, dnd, icat and clipboard find it in both.
# Building afterglow builds aglow too. aglow has no library to find, so it
# gets no run path from CMAKE_INSTALL_RPATH.
set_target_properties(aglow PROPERTIES AUTOMOC OFF AUTOUIC OFF INSTALL_RPATH ""
                      RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:afterglow>")
add_dependencies(afterglow aglow)
install(TARGETS aglow RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${PROJECT_SOURCE_DIR}/data/completions/aglow.bash"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions RENAME aglow)
install(FILES "${PROJECT_SOURCE_DIR}/data/completions/_aglow" DESTINATION ${AFTERGLOW_ZSH_COMPLETION_INSTALL_DIR})
install(FILES "${PROJECT_SOURCE_DIR}/data/completions/aglow.fish"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d)

if(BUILD_TESTING)
    # readelf -d of a static binary lists no NEEDED library.
    if(CMAKE_READELF AND AGLOW_STATIC)
        add_test(NAME aglow_static
                 COMMAND ${CMAKE_COMMAND} -DREADELF=${CMAKE_READELF} -DBINARY=$<TARGET_FILE:aglow>
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckStatic.cmake")
    elseif(AGLOW_STATIC)
        message(STATUS "aglow: readelf was not found, so the aglow_static test is left out (install binutils to add it)")
    endif()

    # A command, its exit status and a pattern of what it prints.
    function(aglow_add_run_test name result regex)
        add_test(NAME ${name}
                 COMMAND ${CMAKE_COMMAND} -DRESULT=${result} "-DREGEX=${regex}" ${AGLOW_RUN_OPTIONS}
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/RunCheck.cmake" -- ${ARGN})
        set_tests_properties(${name} PROPERTIES TIMEOUT 60)
    endfunction()
    string(REPLACE "." "\\." AGLOW_VERSION_REGEX "${PROJECT_VERSION}")
    aglow_add_run_test(aglow_version 0 "^aglow ${AGLOW_VERSION_REGEX} \\(protocol [0-9]+\\)" $<TARGET_FILE:aglow> --version)
    # afterglow COMMAND runs the aglow next to it, which then names itself
    # afterglow COMMAND, and returns its status.
    aglow_add_run_test(afterglow_runs_aglow 0 "Usage: afterglow icat " $<TARGET_FILE:afterglow> icat --help)
    aglow_add_run_test(afterglow_passes_aglow_arguments 0 "Usage: afterglow transfer "
                       $<TARGET_FILE:afterglow> transfer --help)
    aglow_add_run_test(afterglow_passes_aglow_status 1 "Unknown option: --no-such-option"
                       $<TARGET_FILE:afterglow> icat --no-such-option)
    # The usage of run names the program: aglow run, or afterglow run when afterglow runs aglow. It connects to nothing.
    aglow_add_run_test(aglow_run_usage 0 "^Usage: aglow run " $<TARGET_FILE:aglow> run --help)
    aglow_add_run_test(afterglow_runs_aglow_run 0 "^Usage: afterglow run " $<TARGET_FILE:afterglow> run --help)
    # A copy of afterglow alone in a directory has no aglow next to it.
    set(AGLOW_RUN_OPTIONS "-DCOPY_TO=${CMAKE_CURRENT_BINARY_DIR}/without-aglow")
    aglow_add_run_test(afterglow_without_aglow 127 "afterglow icat: cannot run .*/without-aglow/aglow: "
                       $<TARGET_FILE:afterglow> icat --help)
    unset(AGLOW_RUN_OPTIONS)
    # An aglow next to it that cannot run: 126, as a shell answers.
    set(AGLOW_RUN_OPTIONS "-DCOPY_TO=${CMAKE_CURRENT_BINARY_DIR}/aglow-not-executable" -DNOT_EXECUTABLE=ON)
    aglow_add_run_test(afterglow_aglow_not_executable 126
                       "afterglow icat: cannot run .*/aglow-not-executable/aglow: Permission denied"
                       $<TARGET_FILE:afterglow> icat --help)
    unset(AGLOW_RUN_OPTIONS)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # aglow clipboard against a terminal that the test plays in a pseudo-terminal.
        add_test(NAME aglow_clipboard_pty
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/clipboard_pty.py $<TARGET_FILE:aglow>)
        set_tests_properties(aglow_clipboard_pty PROPERTIES TIMEOUT 120)
        # Crafted images (GIF disposal and frame caps, BMP, TGA, PSD, HDR, PNM, PNG, JPEG).
        add_test(NAME aglow_icat_decode
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/icat_decode.py $<TARGET_FILE:aglow>)
        # A TMUX that names no server: the test must keep it from aglow, or aglow
        # wraps its commands for tmux and runs tmux set on the pane of a developer.
        set(AGLOW_FOREIGN_TMUX "TMUX=${CMAKE_CURRENT_BINARY_DIR}/no-tmux-server,0,0")
        set_tests_properties(aglow_icat_decode PROPERTIES TIMEOUT 120 ENVIRONMENT "${AGLOW_FOREIGN_TMUX}")
        add_test(NAME aglow_clipboard_cases
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/clipboard_cases.py $<TARGET_FILE:aglow>)
        # It skips (77) on a kernel that does not show a process that waits in
        # open(), after the other cases ran.
        set_tests_properties(aglow_clipboard_cases PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
        # What icat writes, drawn by the terminal's own renderer. It needs a
        # Vulkan driver, and it skips (77) without one.
        if(AFTERGLOW_BUILD_TOOLS)
            add_test(NAME aglow_icat_render
                     COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/icat_render.py
                             $<TARGET_FILE:aglow> $<TARGET_FILE:afterglow_renderpng>)
            set_tests_properties(aglow_icat_render PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
                                 ENVIRONMENT "${AGLOW_FOREIGN_TMUX}")
        endif()
    endif()
    # The input parser and the command line, without a terminal.
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_executable(aglow_tests tests/AglowInputTest.cpp Cli.cpp Tty.cpp)
    target_link_libraries(aglow_tests PRIVATE Afterglow::Proto GTest::gtest_main)
    set_target_properties(aglow_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
    gtest_discover_tests(aglow_tests TEST_PREFIX "Aglow.")
endif()
