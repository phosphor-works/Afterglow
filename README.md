<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Afterglow

Afterglow is a terminal for Linux and Wayland. It uses Qt and Vulkan to render text and visual effects.

## Features

- Tabs, split panes, pane zoom and saved project layouts.
- Terminal sessions that keep running without a window, with `afterglow mux`.
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
- `aglow`, a static client for Kitty file transfer, drag and drop, images and the clipboard, also on remote hosts.
  It also runs script commands from a shell.

## Install

Afterglow requires Linux, Wayland and a Vulkan driver. Install the build with the prefix of its configuration:

```sh
sudo cmake --install build
```

[Build and install](docs/BUILD.md#install) tells what the installation contains and where each part goes.

## Build

[Build and install](docs/BUILD.md) lists the required software and the build options.

1. Get the source:

   ```sh
   git clone https://github.com/phosphor-works/Afterglow
   cd Afterglow
   ```

2. Configure the build:

   ```sh
   cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
   ```

3. Build the application:

   ```sh
   cmake --build build
   ```

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
- [Mux sessions](docs/MUX.md)
- [Supported protocols and limits](docs/COMPATIBILITY.md)

The [Phosphor scene](examples/phosphor/README.md) is an audio-responsive example.
The [release notes](CHANGELOG.md) list the supported features.

## Contributing

Report a bug or propose a change in the
[Afterglow issues](https://github.com/phosphor-works/Afterglow/issues). Commit messages use Conventional Commits.
Every change follows the
[phosphor-works conventions](https://github.com/phosphor-works/workspace/blob/main/claude/conventions.md).

## License

| Part | License |
| --- | --- |
| The application `afterglow` | GPL-3.0-or-later |
| The mux daemon `aglow-mux` | GPL-3.0-or-later |
| The client `aglow` and the developer tools | GPL-3.0-or-later |
| The libraries in `libs/` | LGPL-2.1-or-later |
| Most bundled shader packs | LGPL-2.1-or-later |
| Third-party themes | MIT |
| The icon | CC-BY-SA-4.0 |

Each file, or `REUSE.toml` for a file without a header, identifies its license.
The [LICENSES directory](LICENSES) contains the license texts of the files in this repository.

The binaries include third-party code:

| Binary | Third-party code (license) |
| --- | --- |
| The binaries | zlib-ng (Zlib license), xxHash (BSD-2-Clause), jsoncons (BSL-1.0, with its grisu3 and a5hash code under MIT), Luau (MIT) and Lua (MIT) |
| `aglow` and `aglow-mux` | zlib-ng and xxHash |
| `aglow` and `aglow-mux` | libpng (libpng-2.0 license), libjpeg-turbo (IJG and BSD-3-Clause licenses), libwebp (BSD-3-Clause) and Little CMS (MIT) |
| `afterglow` and `aglow-mux` | The character tables, from the Unicode Character Database (Unicode-3.0) |
| A static `aglow` | Parts of glibc and of the GCC runtime libraries |

This software is based in part on the work of the Independent JPEG Group.

The installation also includes the license texts of zlib-ng, xxHash, libpng, libjpeg-turbo, libwebp, Little CMS, the
Unicode Character Database, jsoncons, Luau and Lua.
For a static `aglow`, it also includes `LICENSE.glibc` and `LICENSE.libstdc++`.
[Build and install](docs/BUILD.md#install) tells where they go.
