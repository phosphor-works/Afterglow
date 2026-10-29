# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(afterglow_renderpng renderpng.cpp)
# Links the application's shader loading (GPL), so packs load as in the app.
target_link_libraries(afterglow_renderpng PRIVATE afterglow_shaders Afterglow::Render Qt6::Gui)
