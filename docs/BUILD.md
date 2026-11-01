<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Build and install

Afterglow requires Linux, Wayland and a Vulkan driver.
These instructions use the current source layout.

## Required software

- CMake 3.21 or later, pkg-config and a C++20 compiler.
- Qt 6.10 or later. The Phosphor libraries need this version.
- Qt Core, Gui, Widgets, Qml, Quick, QuickControls2, Concurrent, DBus, Svg, WaylandClient and ShaderTools.
- Qt GuiPrivate, ShaderToolsPrivate, WaylandClientPrivate and QuickPrivate headers.
- FreeType, HarfBuzz, FriBidi and fontconfig.
- xkbcommon, wayland-client, `wayland-scanner`, zlib, xxHash and the 32-bit PCRE2 library.
- KF6 WindowSystem.
- GTest, unless you configure with `-DBUILD_TESTING=OFF`.
- The Phosphor libraries from a PlasmaZones checkout.

| Optional software | Function | Result when it is missing |
| --- | --- | --- |
| `rsvg-convert` | Creates the PNG icon sizes | Only the SVG icon is installed. |
| `tic` | Compiles the `afterglow` terminfo entry | The entry is not installed. Do not set `Terminal.term` to `afterglow`. |
| `cava` | Supplies system audio data for shaders | Audio-reactive shaders get no audio. |
| KF6 GlobalAccel | Registers the quick terminal shortcut with KDE global shortcuts | The desktop portal registers the shortcut. |
| `qqc2-desktop-style` | Draws tabs, menus and prompts in the Plasma style | The controls use the Fusion style. |

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
git -C ../PlasmaZones checkout 4d14ed2a5
```

A later PlasmaZones commit can change the Phosphor API. The build then fails.

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
Private libraries go in an `afterglow` directory in the library directory, for example `lib/afterglow/` or `lib64/afterglow/`.
This location does not replace libraries from a system Phosphor installation.

See [testing](TESTING.md) for native desktop test commands.
