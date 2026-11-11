<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Afterglow

Afterglow is a terminal for Linux and Wayland. It uses Qt and Vulkan to render text and visual effects.

## Main features

- Tabs, split panes, pane zoom and saved project layouts.
- Automatic shell integration for Bash, Zsh and Fish.
- Search, keyboard selection, command output and text hints.
- Configurable fonts, colors, keybindings and desktop-themed controls.
- Unicode text, font shaping, color emoji and optional bidirectional text.
- Kitty graphics, Sixel and iTerm2 inline images.
- Shader effects and 3D meshes that respond to system audio, commands and current-tab history.
- Luau scripts, local packages and saved script state.
- Command-line and socket control.
- Accessible terminal text and input methods.
- Kitty file transfers, drag and drop, and file downloads.
- `aglow`, a static client for Kitty file transfer, drag and drop, images and the clipboard, also on remote hosts. It also runs script commands from a shell.

## Documentation

Start with the [documentation index](docs/README.md).

- [Build and install](docs/BUILD.md)
- [Settings and appearance](docs/APPEARANCE.md)
- [Keyboard, mouse, touch, search and shell integration](docs/INPUT.md)
- [Panes and projects](docs/PROJECTS.md)
- [Luau scripting](docs/SCRIPTING.md)
- [Shader effects](docs/SHADERS.md)
- [Desktop integration](docs/DESKTOP.md)
- [External control](docs/CONTROL.md)
- [Supported protocols and limits](docs/COMPATIBILITY.md)

See the [Phosphor scene](data/examples/phosphor/README.md) for an audio-responsive example.
See the [release notes](CHANGELOG.md) for supported features.

## License

The application uses GPL-3.0-or-later. The libraries in `libs/` and the `aglow` client use LGPL-2.1-or-later.
Most bundled shader packs use LGPL-2.1-or-later. Third-party themes use MIT.
The icon uses CC-BY-SA-4.0.
The binaries include zlib-ng (Zlib license), xxHash (BSD-2-Clause), valijson (BSD-2-Clause), Luau (MIT) and Lua (MIT).
`aglow` includes zlib-ng and xxHash.
`aglow` and `afterglow-mux` also include libpng (libpng-2.0 license), libjpeg-turbo (IJG and BSD-3-Clause licenses) and libwebp (BSD-3-Clause).
This software is based in part on the work of the Independent JPEG Group.
A static `aglow` also includes parts of glibc and of the GCC runtime libraries.
Each file, or `REUSE.toml` for a file without a header, identifies its license.
The [LICENSES directory](LICENSES) contains the license texts of the files in this repository.
The installation also includes the license texts of zlib-ng, xxHash, libpng, libjpeg-turbo, libwebp, valijson, Luau and Lua.
For a static `aglow`, it also includes `LICENSE.glibc` and `LICENSE.libstdc++`.
[Build and install](docs/BUILD.md#install) tells where they go.
