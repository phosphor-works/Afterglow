<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Build and install

Afterglow requires Linux, Wayland and a Vulkan driver.
These instructions use the current source layout.

## Required software

- Qt 6.8 or later.
- Qt Core, Gui, Widgets, Qml, Quick, QuickControls2, Concurrent, DBus and ShaderTools.
- Qt GuiPrivate, ShaderToolsPrivate, WaylandClientPrivate and QuickPrivate headers.
- FreeType, HarfBuzz, FriBidi and fontconfig.
- xkbcommon, wayland-client, zlib, xxHash and the 32-bit PCRE2 library.
- KF6 WindowSystem.
- The Phosphor libraries from a PlasmaZones checkout.

| Optional tool | Function |
| --- | --- |
| `rsvg-convert` | Creates icon sizes |
| `tic` | Compiles the terminfo entry |
| GTest | Runs unit tests |
| `cava` | Supplies system audio data for shaders |

## Qt private APIs

The renderer uses Qt private APIs. CMake reports this dependency during configuration.
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

3. Configure the build:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

4. Build the application:

```sh
cmake --build build
```

5. Run the tests:

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
sudo cmake --install build --prefix /usr
```

The installation includes the executable, desktop entry, AppStream metadata, icons, shader packs and terminfo entry.
It also includes the API declarations and examples.
Private libraries go in `lib/afterglow/`.
This location does not replace libraries from a system Phosphor installation.

See [testing](TESTING.md) for native desktop test commands.
