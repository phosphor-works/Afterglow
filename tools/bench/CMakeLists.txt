# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(afterglow_vtbench vtbench.cpp)
target_link_libraries(afterglow_vtbench PRIVATE Afterglow::Vt)
set_target_properties(afterglow_vtbench PROPERTIES AUTOMOC OFF)
