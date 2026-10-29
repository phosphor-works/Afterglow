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
git clone https://github.com/phosphor-works/afterglow
cd afterglow
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

### Themes

`Colors.theme` names a theme; its colours replace the ones in `Colors`.
Themes come from `~/.config/afterglow/themes`, then the installed
`share/afterglow/themes` (`afterglow-dark` and `afterglow-light`). Theme files
can be afterglow JSON (the `Colors` keys), Ghostty themes or kitty themes, so
collections such as iTerm2-Color-Schemes drop straight in. To follow the
desktop's light or dark preference, write `"light:afterglow-light,dark:nord"`.
A path works too, so a theme another tool generates stays in step.

### Keybinds

`Keybinds.bindings` maps an action to a key or a list of keys; an empty
string unbinds it. Besides the built-in actions, `"text:<bytes>"` sends bytes
to the terminal (`\e \r \n \t \\ \xHH \uHHHH`), for example
`"text:\\x1b\\r": "Shift+Return"`, and `"script:<id>.<action>"` runs a script
action.

## Shaders

Shaders are Ghostty-style GLSL (`mainImage`, `iChannel0`, the cursor uniforms)
or phosphor shader packs, listed in five chains under `Shaders`:

- `background`, `text` and `cursor` each run per pane on that layer alone,
  before the layers are put together. Backgrounds include images under the
  text; text includes decorations and images over it. A layer's input is
  premultiplied and transparent where the layer has nothing, and the shader
  must return premultiplied colour too.
- `pane` runs per pane on the finished pane.
- `window` runs once over the whole window.

The tab bar and other chrome are drawn after every chain. With no layer
shaders, the layers are never split apart and cost nothing.

Shaders can also react to what happens in the terminal: `iSignal[0..7]`
hold a value and the time it was set, per pane, set by scripts
(`afterglow.signal`), `afterglow ctl signal` or D-Bus. `data/examples/glasshouse`
uses all of this to dress afterglow for the Glasshouse desktop.

## Control and scripting

Other programs drive afterglow over D-Bus (`io.github.fuddlesworth.Afterglow.Control`
at `/Afterglow/Control`), or with `afterglow ctl`: list panes, send text, split,
open tabs and windows, set titles, notify, run actions, and `watch` events as
JSON lines. Run `afterglow ctl --help` for the full list. Reading a pane's text
needs `"Control": { "allowReadText": true }`; `"Control": { "enabled": false }`
turns the interface off.

Luau scripts in `~/.config/afterglow/scripts/` react to events (commands
finishing, bells, titles, focus) and add actions to the command palette, which
bind in `Keybinds` as `"script:<id>.<action>"`. Scripts run sandboxed, with
no file or process access and a short time limit. They return effects, which
afterglow rate limits and carries out. Types for editors are installed to
`share/afterglow/api/afterglow.d.luau`, and examples to
`share/afterglow/examples/scripts`. `"Scripts": { "enabled": false }` turns
scripting off.

## Licence

The application is GPL-3.0-or-later; the libraries under `libs/` are
LGPL-2.1-or-later; the icon is CC-BY-SA-4.0. Each file names its licence;
the texts are in `LICENSES/`.
