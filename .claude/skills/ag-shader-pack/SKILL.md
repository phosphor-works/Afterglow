---
name: ag-shader-pack
description: "Write, change or preview an Afterglow shader pack (data/shaders, a package's shaders/), or change the shader contract that packs read. Covers the pack layout, the contract headers and their C++ mirrors, previews with afterglow_renderpng, and the performance check. Use when: shader pack, new effect, GLSL, pTerminal, metadata.json, overview pack, tab transition pack, pane effect, preview a shader, render a PNG, uniform layout."
---

<!--
SPDX-FileCopyrightText: 2026 fuddlesworth
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Shader packs

`docs/SHADER-API.md` is the contract for pack authors, and `docs/SHADERS.md` tells users how to configure the
chains. Read the sections that apply before you write GLSL. This skill gives the procedure and the traps.

## A pack

A pack is a directory: `metadata.json`, one or more stages (`effect.frag`, buffer shaders, a vertex stage, mesh
stages) and an optional `README.md`.

- `metadata.json` has `id`, `name`, `category`, `description`, `author`, `version`, `fragmentShader`, and optional
  `parameters` and `presets`. `description` and parameter names are user-facing prose (docs/WRITING.md). Metadata
  files carry no SPDX header; `REUSE.toml` licenses them.
- Each GLSL file starts with the SPDX header. New packs are `LGPL-2.1-or-later`. Keep the license of an existing file.
- A stage defines `vec4 pTerminal(vec2 uv)` and returns premultiplied colour. Afterglow adds `main()`, the
  parameters as `p_<id>`, and `terminal_lib.glsl`.
- Coordinates are top-down. `uv` (0, 0) is the top-left corner of the canvas.
- A pack in a script package must declare the id `<package>.<pack>`. The registry rejects other ids.
- The event chains (`Shaders.overview`, `tabTransition`, `paneEffects`) take one pack id, not a list. They run only
  while their event shows, after the window chain.

## The contract and its mirrors

The contract headers are in `data/shaders/shared/`. Each uniform block there has a C++ mirror in
`libs/afterglow-render/include/AfterglowRender/ShaderProgram.h`, with `static_assert`s on its size and offsets
(`TerminalUniforms` 5184 bytes, `TerminalSceneUniforms` 8192, `TerminalPaneUniforms` 2656).

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
| Animation frames | `--time S`, `--frames N --fps F` |
| Parameters | `--param my-pack:style=1` |
| Several panes | `--split`, `--grid 3x2` |
| Overview | `--overview pane-deck --tabs 3 --highlight 4 --overview-position 3.5`; closing: `--overview-closing --overview-amount 0.6` |
| Tab switch | `--tab-transition tab-switch --transition-progress 0.4 --transition-direction -1` |
| Pane effects | `--pane-effects pane-effects --open-progress 0.5 --open-edge 2`, `--close-progress`, `--context 1`, `--pulse 1:0.5` |
| Audio | `--audio-bands 0.8,0.4,0.2` |
| A package's packs | `--package data/examples/packages/scripted-overview --overview scripted-overview.wall` |
| Frame time | `--timings`, with `--pixels 3840x2160` for 4K |

Check the edge cases, not only the middle of an animation: progress 0 and 1, one pane, two panes, many panes (the
ring and the grid), and the wrap point of anything that loops.

## Performance

A window chain shades every pixel of the window, every frame, while it runs. Measure with `--timings` at
`--pixels 3840x2160`, before and after, and put both numbers in the commit message. Work per pixel that loops over
all panes or cards is the usual cost. Find the few candidates first, then shade only those. Reading `iTime` makes the
window render continuously. Event ages (`cursorAge()`, `focusAge()`, `signalAge()`, `uSignal`) keep it rendering for
`settleSeconds` after each event, so read them only when the effect needs them.

## Done

- `ctest --test-dir build -R 'Render|Shader'` passes.
- The previews at the edge cases look right.
- `docs/SHADERS.md` lists a new bundled pack, and `CHANGELOG.md` mentions it.
