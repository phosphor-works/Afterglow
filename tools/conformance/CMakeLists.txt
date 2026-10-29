# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# Runs a program in a pty against the bare terminal core, for conformance
# suites (esctest) that drive the terminal and read its replies.
add_executable(afterglow_headless headless.cpp)
target_link_libraries(afterglow_headless PRIVATE Afterglow::Vt util)
set_target_properties(afterglow_headless PROPERTIES AUTOMOC OFF)
