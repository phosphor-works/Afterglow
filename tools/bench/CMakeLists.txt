# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(afterglow_vtbench vtbench.cpp)
target_link_libraries(afterglow_vtbench PRIVATE Afterglow::Vt)
set_target_properties(afterglow_vtbench PROPERTIES AUTOMOC OFF)
