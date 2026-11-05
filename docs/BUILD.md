<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Build and install

Afterglow requires Linux, Wayland and a Vulkan driver.
These instructions use the current source layout.

## Required software

- CMake 3.21 or later, pkg-config, a C compiler and a C++20 compiler.
- Qt 6.10 or later. The Phosphor libraries need this version.
- Qt Core, Gui, Widgets, Qml, Quick, QuickControls2, Concurrent, DBus, Network, Svg, WaylandClient and ShaderTools.
- Qt GuiPrivate, ShaderToolsPrivate, WaylandClientPrivate and QuickPrivate headers.
- FreeType, HarfBuzz, FriBidi and fontconfig.
- xkbcommon, wayland-client, `wayland-scanner` and the 32-bit PCRE2 library.
- KF6 WindowSystem.
- GTest, unless you configure with `-DBUILD_TESTING=OFF`.
- The Phosphor libraries from a PlasmaZones checkout.
- The static C and C++ runtime libraries (`libc.a` and `libstdc++.a`), for `aglow`.
  Fedora puts them in the packages `glibc-static` and `libstdc++-static`.
  Without them, the configuration stops with a message.
  To build without `aglow`, configure with `-DAFTERGLOW_BUILD_AGLOW=OFF`.

The build compiles zlib-ng and xxHash from their [source archives](#source-archives).
These are C sources, so the build needs the C compiler.
It does not use the copies that the system has.

| Optional software | Function | Result when it is missing |
| --- | --- | --- |
| `rsvg-convert` | Creates the PNG icon sizes | Only the SVG icon is installed. |
| `tic` | Compiles the `afterglow` terminfo entry | The entry is not installed. Do not set `Terminal.term` to `afterglow`. |
| `cava` | Supplies system audio data for shaders | Audio-reactive shaders get no audio. |
| KF6 GlobalAccel | Registers the quick terminal shortcut with KDE global shortcuts | The desktop portal registers the shortcut. |
| `qqc2-desktop-style` | Draws tabs, menus and prompts in the Plasma style | The controls use the Fusion style. |

## Source archives

The first configuration downloads source archives. It needs network access one time.

| Archive | Pin file | Function |
| --- | --- | --- |
| CPM.cmake | Both pin files | Downloads the other archives |
| Luau | `../PlasmaZones/cmake/PhosphorDependencies.cmake` | Runs the Luau scripts |
| valijson | `../PlasmaZones/cmake/PhosphorDependencies.cmake` | Checks Phosphor JSON files against their schemas |
| zlib-ng | `cmake/AfterglowDependencies.cmake` | Compresses and decompresses images and file transfers |
| xxHash | `cmake/AfterglowDependencies.cmake` | Calculates the block hashes of delta file transfers |

The repository includes `tools/aglow/stb/stb_image.h`, which `aglow icat` uses to decode images.
`cmake/AfterglowDependencies.cmake` pins the file to an upstream commit and its SHA-256 hash.
A configuration with a changed file stops with an error.

CMake checks each archive against the SHA-256 hash in its pin file.
The archives go into the CPM cache.
By default, every build directory uses the same cache.
The first row of this table that applies selects the cache:

| Condition | CPM cache |
| --- | --- |
| The configuration command has `-DCPM_SOURCE_CACHE=/path/to/cache` | `/path/to/cache` |
| The `CPM_SOURCE_CACHE` environment variable is set | The directory in the variable |
| The configuration command has `-DPHOSPHOR_DEPS_ARCHIVES` and no `-DAFTERGLOW_DEPS_ARCHIVES` | `.cache/cpm` in the Afterglow directory |
| No other row applies | `.cache/cpm` in the PlasmaZones checkout |

A build directory keeps the CPM cache of its first configuration.
To change the cache of a build directory, give `-DCPM_SOURCE_CACHE`.
When the cache has all archives, a configuration needs no network access.

To build on a computer without network access, prepare the source on a computer with network access:

1. Do steps 1 to 3 of [Build](#build).

2. In the Afterglow directory, download the Phosphor archives into a directory:

```sh
cmake -DPHOSPHOR_DEPS_FETCH=/path/to/archives -P ../PlasmaZones/cmake/PhosphorDependencies.cmake
```

3. Download the Afterglow archives into the same directory:

```sh
cmake -DAFTERGLOW_DEPS_FETCH=/path/to/archives -P cmake/AfterglowDependencies.cmake
```

4. Copy the Afterglow and PlasmaZones directories into one parent directory on the build computer.

5. Copy the archive directory to the build computer.

6. On the build computer, configure the build as in step 4 of [Build](#build), with these options:

```sh
-DPHOSPHOR_DEPS_ARCHIVES=/path/to/archives -DAFTERGLOW_DEPS_ARCHIVES=/path/to/archives
```

7. Do steps 5 and 6 of [Build](#build).

With the two options, the configuration downloads nothing.
If you set only one option, the configuration downloads CPM.cmake and the archives of the other pin file.

## Qt private APIs

Afterglow uses Qt private APIs. CMake reports this dependency during configuration.
These APIs are tied to the installed Qt build.
Rebuild Afterglow after a Qt update.
Do not run an existing binary against a different Qt private-module build.

## Build

1. Get the Afterglow source:

```sh
git clone https://github.com/phosphor-works/Afterglow
cd Afterglow
```

2. Get the Phosphor source in the adjacent directory:

```sh
git clone https://github.com/fuddlesworth/PlasmaZones ../PlasmaZones
```

3. Select the PlasmaZones commit that this Afterglow source was tested with:

```sh
git -C ../PlasmaZones checkout 4e5aca5c9
```

A later PlasmaZones commit can change the Phosphor API. The build then fails.
An older PlasmaZones checkout has no `cmake/PhosphorDependencies.cmake`. The configuration then stops with an error.

4. Configure the build with the installation prefix:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
```

The executable stores the shell integration, terminfo, shader, theme and plugin paths of this prefix.
Without GTest, add `-DBUILD_TESTING=OFF` to the command, and skip step 6.

5. Build the application:

```sh
cmake --build build
```

6. Run the tests:

```sh
ctest --test-dir build --output-on-failure
```

To use another Phosphor checkout, add this option to the configuration command:

```sh
-DAFTERGLOW_PHOSPHOR_LIBS_DIR=/path/to/PlasmaZones/phosphor-libs
```

The build compiles the Phosphor libraries with Afterglow.

## Install

Run this command to install the build:

```sh
sudo cmake --install build
```

Use the prefix of the configuration step. Do not give a different `--prefix` here.
If you do, shell integration is off, and the executable can miss its plugin, shader packs and themes.

The installation includes the executable, desktop entry, AppStream metadata, icons, shader packs, themes and terminfo entry.
It also includes the shell integration scripts, the API declarations, the examples and the license texts.
It installs `aglow` in the same directory as `afterglow`, with its Bash, Zsh and Fish completions and the stb_image license as `LICENSE.stb_image`.
A build with sanitizers, such as the `debug` preset, links `aglow` dynamically, because a static program cannot use them.
To build without `aglow`, add `-DAFTERGLOW_BUILD_AGLOW=OFF` to the configuration command.
Private libraries go in an `afterglow` directory in the library directory, for example `lib/afterglow/` or `lib64/afterglow/`.
This location does not replace libraries from a system Phosphor installation.

See [testing](TESTING.md) for native desktop test commands.
