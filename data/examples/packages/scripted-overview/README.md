<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Scripted overview

This package ships a script and the shader that it drives.

- `init.luau` opens the pane overview and takes the keyboard while it is open.
  Keys a and d, h and l, or the Left and Right arrow keys move the highlight.
  Keys w and s jump between tabs.
  Enter or Space chooses a pane. Keys q or Escape close the overview.
- `shaders/wall` draws the overview as a grid of live cards on a curved wall.
  Its ID is `scripted-overview.wall`.

## Install

1. Copy this folder to `~/.config/afterglow/scripts/scripted-overview`.
2. Run **Use Package Shaders…** from the command palette.
3. Select `Shaders.overview = scripted-overview.wall`.
4. Bind `script:scripted-overview.open` to a key in `Keybinds.bindings`.

The shader is available only while the package is loaded.
If you remove the package, the overview setting refers to a missing pack, and the overview does not open.
