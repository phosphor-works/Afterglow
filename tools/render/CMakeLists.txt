# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

# OverlaySpec is compiled in, so that a popup is placed by the window's rules.
add_executable(afterglow_renderpng renderpng.cpp ${PROJECT_SOURCE_DIR}/afterglow/src/scripting/OverlaySpec.cpp)
# Links the application's shader loading (GPL), so packs load as in the app.
target_link_libraries(afterglow_renderpng PRIVATE afterglow_shaders Afterglow::Render Qt6::Gui)
