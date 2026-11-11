# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# afterglow_vtdiff compares the terminal core with libghostty-vt
# (docs/TESTING.md, "Differences with libghostty-vt"). libghostty-vt is not a
# dependency of Afterglow: build it on its own and point CMake at its prefix,
#   cmake -B build -DAFTERGLOW_BUILD_VTDIFF=ON -DCMAKE_PREFIX_PATH=/path/to/libghostty
# The build fetches nothing.

find_package(ghostty-vt 1.0 CONFIG REQUIRED)

add_executable(afterglow_vtdiff vtdiff.cpp)
target_link_libraries(afterglow_vtdiff PRIVATE Afterglow::Vt ghostty-vt::ghostty-vt)
target_compile_features(afterglow_vtdiff PRIVATE cxx_std_20)
