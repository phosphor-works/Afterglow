# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# aglow: kitty's file transfer, drag and drop, graphics and clipboard
# protocols from the command line, as one static binary. It uses only
# libs/afterglow-proto (with its bundled zlib-ng and xxHash) and the vendored
# stb_image: no Qt and no Phosphor, so that it builds alone for a remote host.

afterglow_vendored_file(stb_image AGLOW_STB_IMAGE)
get_filename_component(AGLOW_STB_DIR "${AGLOW_STB_IMAGE}" DIRECTORY)

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
target_link_options(aglow PRIVATE -static)
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
    if(AGLOW_READELF)
        add_test(NAME aglow_static
                 COMMAND ${CMAKE_COMMAND} -DREADELF=${AGLOW_READELF} -DBINARY=$<TARGET_FILE:aglow>
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckStatic.cmake")
    endif()
    add_test(NAME aglow_version COMMAND aglow --version)
    set_tests_properties(aglow_version PROPERTIES PASS_REGULAR_EXPRESSION "^aglow ${PROJECT_VERSION} \\(protocol [0-9]+\\)")
    # What icat writes, drawn by the terminal's own renderer (which needs a Vulkan driver).
    if(AFTERGLOW_BUILD_TOOLS)
        find_package(Python3 COMPONENTS Interpreter)
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
