# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(afterglow_renderpng renderpng.cpp)
# Links the application's shader loading (GPL), so packs load as in the app,
# its overlay options, so a popup is placed by the window's rules, its image
# decoder, so images decode within the panes' limits, and its settings, for
# the defaults of the window's look.
target_link_libraries(afterglow_renderpng PRIVATE afterglow_shaders afterglow_overlayspec afterglow_imagedecode afterglow_config
    Afterglow::Render Qt6::Gui)
