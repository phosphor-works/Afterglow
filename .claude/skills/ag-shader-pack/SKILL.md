---
name: ag-shader-pack
description: "Write, change or preview an Afterglow shader pack (data/shaders, examples/shaders, a package's shaders/), or change the shader contract that packs read. Covers the pack layout, the contract headers and their C++ mirrors, previews with afterglow_renderpng, and the performance check. Use when: shader pack, new effect, GLSL, pTerminal, metadata.json, overview pack, tab transition pack, pane effect, overlay transition, overlay fade, preview a shader, render a PNG, uniform layout."
---

<!--
SPDX-FileCopyrightText: 2026 fuddlesworth
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Shader packs

`docs/SHADER-API.md` is the contract for pack authors, and `docs/SHADERS.md` tells users how to configure the
chains. Read the sections that apply before you write GLSL. This skill gives the procedure and the traps.

## Where a pack goes

| Directory | Packs | Installs |
| --- | --- | --- |
| `data/shaders/` | The Phosphor packs (the defaults of the event chains, and Phosphor Depths), and the neutral packs: Plain Overview, Plain Tabs, Plain Panes, Overlay Fade, Bloom and Cursor Trail | Yes |
| `examples/shaders/` | Packs that show the API or another look (CRT Curve, Tilt, Pane Deck, Tab Switch) | No |
| `examples/packages/*/shaders/` | The packs of an example script package | No |

A Phosphor pack follows the Phosphor design language. A position moves on the host's spring, and a color
changes with a fade. An edge that changes is bright at once, and then its glow fades. Nothing turns in 3D, and nothing
loops to get attention. The colors are the spectrum (cyan, blue, purple, rose) from `shared/phosphor_spectrum.glsl`,
with the `followTheme` and `stop0` to `stop3` parameters. A neutral pack has no color of its own: it takes colors
from the theme. The motion of an event pack is in `shared/flat_overview.glsl`, `flat_tabs.glsl` and `flat_panes.glsl`.
A Phosphor pack and its plain pack both include that file, and differ only in the macros that they define before the
include: change the motion there, one time, for both. A default of `Shaders.*` names only a pack of
`data/shaders/` (`ConfigTest` checks it). A test or a tool that needs an example pack copies it into the `shaders`
directory of its configuration (`installExamplePack` in `NativeWindowTest.cpp`), or names `--shader-dir examples/shaders`.

## A pack

A pack is a directory: `metadata.json`, one or more stages (`effect.frag`, buffer shaders, a vertex stage, mesh
stages) and an optional `README.md`.

- `metadata.json` has `id`, `name`, `category`, `description`, `author`, `version`, `fragmentShader`, and optional
  `parameters` and `presets`. `description` and parameter names are user-facing prose (docs/WRITING.md). Metadata
  files carry no SPDX header; `REUSE.toml` licenses them.
- Each GLSL file starts with the SPDX header. New packs are `LGPL-2.1-or-later`. Keep the license of an existing file.
- A stage defines `vec4 pTerminal(vec2 uv)` and returns premultiplied colour. Afterglow adds `main()`, the
  parameters as `p_<id>`, and `terminal_lib.glsl`. It looks for `pTerminal` in the stage's own file, before it
  expands the includes: an entry point in an include file gives "Link failed" with no other message.
- A final pass that moves the content (a bend, a lens, a perspective) also defines `vec2 pPointerMap(vec2 uv)`,
  through the same helper as `pTerminal`, so clicks reach the cell drawn under the pointer
  (docs/SHADER-API.md#pointer-maps). Add the pack to `PointerMapTest`, with parameter values that move its
  content. `afterglow_renderpng --pointer X,Y` prints where a click lands.
- Coordinates are top-down. `uv` (0, 0) is the top-left corner of the canvas.
- A pack in a script package must declare the id `<package>.<pack>`. The registry rejects other ids.
- The event chains (`Shaders.overview`, `tabTransition`, `paneEffects`, `overlayTransition`) take one pack id, not a
  list. They run only while their event shows. The first three run after the window chain, over the window.
  `overlayTransition` runs over one pane while its overlay fades, after the pane chain and before the window chain.
  Its input is one pane, so `paneTexel()` does not work there: read the pane under the overlay with
  `overlayHostTexel()`.

## The contract and its mirrors

The contract headers are in `data/shaders/shared/`. Each uniform block there has a C++ mirror in
`libs/afterglow-render/include/AfterglowRender/ShaderProgram.h`, with `static_assert`s on its size and offsets
(`TerminalUniforms` 5200 bytes, `TerminalSceneUniforms` 8192, `TerminalPaneUniforms` 2656).

To change the contract:

1. Change the GLSL block and the C++ struct together. Keep std140 alignment: a `vec3` takes 16 bytes, and an array
   element takes at least 16 bytes.
2. Update the `static_assert` values. Never delete one to make the build pass.
3. Fill the new field where the renderer uploads the block (`PostChain.cpp`, `GridRenderer.cpp`).
4. Add a helper in the header if packs will read the field, and document it in `docs/SHADER-API.md`.
   A new contract header also goes in the `qt_add_resources` list of `libs/afterglow-render/CMakeLists.txt`.
5. Add the option to `tools/render/renderpng.cpp` that sets it, so a preview can show it.
6. Add or extend a test in `libs/afterglow-render/tests/RenderTest.cpp`.

The binding table (0 UBO, 1 `uTerminal`, 2–9 `iChannel`, 10 audio, 11–14 `uTexture0`–`3`, 16 depth,
17 `uPreviousText`, 18 scene, 19 panes, 20 `uTabImage`, 21 `uTabAtlas`) is fixed. A new binding needs a free number
in the shader, the renderer and the docs.

## Preview

Build the tools (`AFTERGLOW_BUILD_TOOLS=ON`, which `build` has), then render:

```sh
printf 'hello \e[31mred\e[0m\n' | build/tools/render/afterglow_renderpng --cols 80 --rows 24 \
    --shader my-pack - <scratchpad>/my-pack.png
```

Then read the PNG with the Read tool and look at it. Useful options (`--help` lists all of them):

| Need | Options |
| --- | --- |
| Animation frames | `--time S`, `--frames N --fps F`; `iDate` is `--date 2026-01-01T00:00:00` unless you give another |
| Parameters | `--param my-pack:style=1` |
| Several panes | `--split`, `--grid 3x2`, with the window's dividers and `--unfocused-dim F` over the unfocused panes |
| Script docks | `--dock EDGE:CELLS:TEXT`, for example `--dock top:1:$'\e[44m main \e[0m'` (repeatable; EDGE is `top`, `bottom`, `left` or `right`; CELLS is 1 to 8 rows for `top` and `bottom`, 1 to 80 columns for `left` and `right`) |
| Overview | `--overview phosphor-overview --tabs 3 --highlight 4 --overview-position 3.5`; closing: `--overview-closing --overview-amount 0.6`; a highlight on its way from another pane: `--highlight 4 --highlight-from 0 --overview-position 2` |
| Tab switch | `--tab-transition phosphor-tabs --transition-progress 0.4 --transition-direction -1` |
| Pane effects | `--pane-effects phosphor-panes --open-progress 0.5 --open-edge 2`, `--close-progress`, `--context 1`, `--pulse 1:0.5` (4 to 255: a package's pulse) |
| Overlay transition | `--overlay $'\e[42m picker \e[0m' --overlay-progress 0.5`, with `--overlay-transition overlay-fade` (the default, `""` for none) |
| Audio | `--audio-bands 0.8,0.4,0.2` |
| A package's packs | `--package examples/packages/scripted-overview --overview scripted-overview.wall` |
| An example pack | `--shader-dir examples/shaders --pane-shader crt-curve` (the user's own packs then do not load) |
| Frame time | `--timings`, with `--pixels 3840x2160` for 4K |

Check the edge cases, not only the middle of an animation: progress 0 and 1, one pane, two panes, many panes (the
ring and the grid), and the wrap point of anything that loops.

## Performance

A window chain shades every pixel of the window, every frame, while it runs. Measure with `--timings` at
`--pixels 3840x2160`, before and after, and put both numbers in the commit message. Work per pixel that loops over
all panes or cards is the usual cost. Find the few candidates first, then shade only those. Reading `iTime` makes the
window render continuously. Event ages (`cursorAge()`, `focusAge()`, `signalAge()`, `uEventAges`, `uSignal`) keep it
rendering for `settleSeconds` after each event, so read them only when the effect needs them.

## Done

- `ctest --test-dir build -R 'Render|Shader'` passes.
- The previews at the edge cases look right.
- `docs/SHADERS.md` lists a new bundled pack, `examples/shaders/README.md` a new example pack, and `CHANGELOG.md`
  mentions it.
