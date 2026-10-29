<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Afterglow

A GPU-accelerated terminal for Wayland, built on Qt and Vulkan. Every pane of
every window draws through one GPU device.

## Requirements

- Qt 6.8 or later (Core, Gui, Qml, Quick, DBus, ShaderTools, and the GuiPrivate,
  WaylandClientPrivate and QuickPrivate headers)
- A Vulkan driver
- FreeType, HarfBuzz, fontconfig, xkbcommon, wayland-client, zlib
- KF6 WindowSystem
- The phosphor libraries from a PlasmaZones checkout (see below)
- Optional: `rsvg-convert` (renders the icon sizes), `tic` (compiles the
  terminfo entry), GTest (tests)

## Building

The phosphor libraries are built from source alongside afterglow. By default
the build expects a PlasmaZones checkout next to this one:

```sh
git clone https://github.com/fuddlesworth/PlasmaZones ../PlasmaZones
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Point `-DAFTERGLOW_PHOSPHOR_LIBS_DIR=/path/to/PlasmaZones/phosphor-libs` at
another checkout if needed.

## Installing

```sh
sudo cmake --install build --prefix /usr
```

This installs `bin/afterglow`, the desktop entry, AppStream metadata, icons,
shader packs and the `afterglow` terminfo entry. Afterglow's libraries and its
own copies of the phosphor libraries go to a private `lib/afterglow/`, so a
system install of phosphor (from PlasmaZones) is left alone.

## Configuration

`~/.config/afterglow/config.json`, reloaded live. Shader packs come from the
installed `share/afterglow/shaders` and `~/.config/afterglow/shaders`.
