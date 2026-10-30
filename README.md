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
- Shader effects that respond to system audio, commands and current-tab history.
- Luau scripts, local packages and saved script state.
- Command-line and D-Bus control.
- Accessible terminal text, input methods, file drops and file downloads.

## Documentation

Start with the [documentation index](docs/README.md).

- [Build and install](docs/BUILD.md)
- [Settings and appearance](docs/APPEARANCE.md)
- [Keyboard, search and shell integration](docs/INPUT.md)
- [Panes and projects](docs/PROJECTS.md)
- [Luau scripting](docs/SCRIPTING.md)
- [Shader effects](docs/SHADERS.md)
- [Desktop integration](docs/DESKTOP.md)
- [Supported protocols and limits](docs/COMPATIBILITY.md)

See the [Phosphor scene](data/examples/phosphor/README.md) for an audio-responsive example.
See the [release notes](CHANGELOG.md) for changes.

## License

The application uses GPL-3.0-or-later. The libraries in `libs/` use LGPL-2.1-or-later.
The icon uses CC-BY-SA-4.0. Each file identifies its license.
The [LICENSES directory](LICENSES) contains the license texts.
