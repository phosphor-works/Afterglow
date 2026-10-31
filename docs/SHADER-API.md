<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Shader pass API

Afterglow uses Phosphor ShaderRegistry metadata, source containment, include resolution, parameter slots and presets.
Terminal shaders are a Phosphor shader family, like Phosphor's overlay, animation, pointer and surface families.
Afterglow's PostChain renders them in terminal QRhi composition passes.
Afterglow reads its own metadata keys from pack metadata: `highDynamicRange`, `windowBackground` and `geometryPasses`.

## The terminal contract

The contract is the set of headers in [data/shaders/shared](../data/shaders/shared).
`terminal_uniforms.glsl` declares every input and documents each one.
Afterglow compiles the copy built into it, so a pack cannot replace these headers.

| Header | Contents | Include |
| --- | --- | --- |
| `terminal_uniforms.glsl` | Uniform block, `uTerminal`, `uTexture0` to `uTexture3` and cursor shape constants. | Automatic. |
| `terminal_color.glsl` | sRGB and linear conversions. | Automatic. |
| `terminal_lib.glsl` | The two headers above and helper functions. | Automatic. |
| `terminal_multipass.glsl` | `iChannel0` to `iChannel7` and `uDepthBuffer`. | For buffers. |
| `terminal_audio.glsl` | `uAudioSpectrum` and the audio helpers. | Enables audio capture. |
| `terminal_history.glsl` | `uPreviousText`. | Enables text history. |

### Entry points

Each stage defines one function. Afterglow adds `terminal_lib.glsl`, the parameter definitions and `main()`.

| Stage | Function |
| --- | --- |
| Final and buffer passes | `vec4 pTerminal(vec2 uv)` returns premultiplied colour for `uv`. |
| `vertexShader` | `vec4 pVertex(vec2 uv)` returns the clip position of the canvas quad's corner at `uv`. |
| Mesh vertex shader | `vec4 pMesh()` returns the clip position of the current vertex. |

A stage can define `main()` instead. Afterglow then adds only the parameter definitions.
That stage must declare `#version 450`, its includes and its outputs.

### Coordinates

All coordinates are top-down. Y increases downwards in positions, rectangles and texture coordinates.
`uv` covers the canvas from (0, 0) at its top-left corner to (1, 1) at its bottom-right corner.
The canvas is the layer, pane, window or buffer that the pass shades.
Canvas pixels are device pixels, scaled by `bufferScale` in a buffer pass.
Use `terminalPixel(uv)` and `terminalUv(pixel)` to convert.
Sample the canvas with `terminalTexel(uv)`.

### Inputs

The uniform block starts with Phosphor's `BaseUniforms` and continues with the terminal fields.

| Input | Meaning |
| --- | --- |
| `iTime`, `iTimeDelta`, `iFrame` | Shader time, wrapped every 1,024 seconds, the frame interval and the frame number. |
| `iResolution` | Canvas size in canvas pixels. |
| `iMouse` | Pointer in canvas pixels (`.xy`) and normalized (`.zw`). All components are -1 off the canvas. |
| `uWindowMouse` | Pointer in window pixels, for a scene that spans panes. -1 outside the window. |
| `customParams`, `customColors` | Parameter values. Use the `p_<id>` names. |
| `uTerminalRect`, `uWindowSize` | The shaded region in window pixels and the window size. Buffer scaling does not change them. |
| `uCellSize` | One character cell in canvas pixels. |
| `uCursorRect`, `uPreviousCursorRect` | The cursor and its previous position, in canvas pixels. |
| `uCursorColor`, `uPreviousCursorColor` | The cursor colours as drawn. |
| `uCursorState` | Shape, previous shape, visibility and pane focus. Shapes are the `kCursor` constants. |
| `uEventAges` | Seconds since the cursor moved and since focus changed. |
| `uBackgroundColor`, `uForegroundColor` | Theme colours. `uBackgroundColor.a` is the window background opacity. |
| `uThemeCursorColor`, `uThemeCursorTextColor` | Theme cursor colours. |
| `uSelectionBackgroundColor`, `uSelectionForegroundColor` | Theme selection colours. |
| `uPalette` | The 256-colour palette. |
| `uSignal`, `uScrollback`, `uAudioLevels` | Script signals, history counts and audio levels. See [Shaders](SHADERS.md). |

Helpers in `terminal_lib.glsl` include `cursorVisible()`, `paneFocused()`, `cursorCenter()`, `cursorAge()`, `focusAge()`, `signalValue()`, `signalAge()` and `paletteColor()`.
Reading time, event ages or signals makes the window render continuously while the shader is active.

## Metadata

| Field | Supported values |
| --- | --- |
| `multipass` | `true` enables multiple passes. |
| `bufferShaders` | Up to eight buffer shaders. |
| `bufferFeedback` | Retain previous buffer values. |
| `bufferScale` | 0.125–1. |
| `halfFloatBuffers` | RGBA16F by default. `false` selects clamped RGBA8. |
| `bufferWrap`, `bufferWraps` | `clamp`, `repeat`, `mirror`. |
| `bufferFilter`, `bufferFilters` | `linear`, `nearest`. |
| `depthBuffer` | `true` adds an auxiliary color attachment to each buffer pass. |
| `vertexShader` | Path to a stage that defines `pVertex`. |
| `geometryPasses` | One mesh object or `null` per buffer shader. |
| `highDynamicRange` | `true` keeps values above 1 between passes. |
| `windowBackground` | `true` shares one background across the window's panes. |

## Pass order and coordinates

`uTerminal` contains the current terminal or layer input in every pass.
Include `terminal_multipass.glsl` to read the buffers as `iChannel0` through `iChannel7`.
Passes execute in metadata order.
Earlier buffers contain the current frame's result. The current and later buffers contain the previous frame.
Without feedback, previous values are cleared each frame.
The final image pass receives all current-frame buffers.

Input textures are local to the region, and every texture is top-down.
Raw `texelFetch` addresses local texture pixels.

Buffer passes use scaled buffer pixels for `iResolution`, cell, cursor and mouse coordinates.
The final pass uses full-resolution pixels.
`uTerminalRect` keeps the pane rectangle in window pixels: X, Y, width and height.
`uWindowSize` keeps the window width and height.
Buffer scaling does not change these two inputs. Use them for a camera or effect that spans split panes.
Buffer `iFrame` starts at zero after reset. Final-pass `iFrame` keeps the window frame counter.
`iTime` is the shared host clock.

Set `"windowBackground": true` on a background pack to share
one set of buffers across the window. The buffer passes run once per frame.
The image pass uses window coordinates and is clipped to visible panes.
Split changes do not resize these buffers or reset their history.
Window resize, shader reload, and changes to source colors, cell size or opacity still reset history.
The focused pane supplies the palette, cursor, signals and parameter overrides.
If no pane has focus, the first visible pane supplies these values.
This option has no effect in text, cursor, pane or window chains.
Use it for scene backgrounds and their lens passes, including effects that sample across split boundaries.

## Auxiliary depth

With depth enabled, write `depthBuffer = vec4(...)` in `pTerminal`.
`uDepthBuffer` samples the preceding pass's auxiliary output.
The first buffer samples its own previous depth. The image pass samples the final buffer's depth.
Initial depth values are zero.
This color attachment stores arbitrary shader data.
Mesh passes use a separate hardware depth attachment for visibility tests.
The two attachments have different purposes.

## Vertex stage

A vertex file defines:

```glsl
vec4 pVertex(vec2 uv)
```

It returns the clip position of the canvas quad's corner at `uv`, from -1 to 1.
`uv` starts at the top-left and ranges from 0 to 1.
`terminalQuadPosition(uv)` is the undeformed position.
The stage can use terminal inputs, parameters and samplers.
Afterward, the host maps the position into the current pane. Scissoring still isolates panes.
Buffer passes use fullscreen geometry unless `geometryPasses` selects a mesh.
The vertex stage applies only to the final pass.

## Mesh passes

A buffer pass can draw triangle meshes with hardware depth testing and depth writes.
The final pass samples the result through `iChannel0` to `iChannel7`.
It can combine mesh output with terminal content and other effects.
Existing ray-marched shaders continue to work.

Add `geometryPasses` to pack metadata. Supply one entry per `bufferShaders` entry.
Use `null` for a fullscreen pass.

```json
{
  "fragmentShader": "effect.frag",
  "multipass": true,
  "bufferShaders": ["surface.frag", "blur.frag"],
  "geometryPasses": [
    {"vertexShader": "mesh.vert", "mesh": "model.obj", "instances": 4},
    null
  ]
}
```

A mesh vertex shader defines `vec4 pMesh()`, which returns the clip position.
Imported meshes provide `meshPosition` (`vec3`), `meshNormal` (`vec3`) and `meshUV` (`vec2`).
Positions are in model coordinates. The shader applies model, camera and projection transforms.
The output uses Vulkan clip coordinates: X and Y from -W to W, and Z from 0 to W.
Positive Y points down. Smaller depth values are closer to the camera.
The rasterizer clips triangles and interpolates attributes with perspective correction.

Declare matching `layout(location=N) out` and `in` values in vertex and fragment shaders.
The fragment shader defines `pTerminal` as usual.
It must output premultiplied color. Face culling and alpha blending are off.
Use `discard` for cutout materials. Transparent surfaces require a separate composition method.

Replace `mesh` with `vertexCount` to generate vertices in the shader.
Use `gl_VertexIndex` to select a vertex and `gl_InstanceIndex` to select an object.
`vertexCount` must be a multiple of three.
Both modes support `instances`, which defaults to one.
Set `samples` to 1, 2, 4 or 8 to select multisample antialiasing. The default is 1.
The renderer resolves multisample color before another pass samples it.
An unsupported sample count uses 1. Multisampling increases buffer memory use.
Use `bufferScale` to reduce the target size when necessary.
The vertex shader can apply parent and child transforms for an object hierarchy.
Afterglow does not maintain a separate CPU scene graph or import OBJ materials.

Both shader stages can read terminal uniforms, audio, images and script-controlled parameters.
Reads of time, audio, scrollback and previous text have the same behavior as fullscreen shaders.
Parameters update without recompiling or uploading the mesh again.

Each mesh pass has its own depth attachment, cleared to 1 before each draw.
All instances in that pass share depth. Separate passes do not share visibility tests.
Depth comparison uses strict `less`. Surfaces at equal depth keep the first fragment.
Feedback retains color and auxiliary data, but does not retain hardware depth.
To pass visible depth to another shader, enable `depthBuffer` and write
`depthBuffer = vec4(gl_FragCoord.z)` in the mesh fragment shader.

OBJ files must contain triangles. Export triangulated faces from the modeling application.
The loader accepts positions, UVs, normals and negative face indices.
Each OBJ line permits up to 4,096 bytes. Coordinate components must be finite and no greater than 1,000,000 in magnitude.
It generates face normals when normals are absent. Missing UVs are zero.
Object names, group names, smoothing groups and material references do not change rendering.
The loader does not open material files. Supply materials in the shader and image parameters.

Geometry assets must be regular files inside the pack, including symlink targets.
Each asset has a 32 MiB file limit. A pack can contain up to 96 MiB of decoded vertices.
A pass permits 3,145,728 vertices, 4,096 instances and 12,582,912 total vertex invocations.
Geometry and hardware depth count toward the shared 256 MiB buffer budget.
A pass that exceeds that budget supplies transparent output.
Resize and shader replacement release obsolete GPU resources.

[Mesh Reactor](../data/shaders/mesh-reactor/README.md) demonstrates instanced geometry,
parent and child rotations, a perspective camera, lighting and audio response.

## Images

Up to four `image` or `texture` parameters use `uTexture0` through `uTexture3`. Use their `p_<id>` names.
`iTextureResolution` holds their sizes.
Defaults, live updates, presets and reset use the same path checks.
An empty path is transparent.
Relative paths, including symlink targets, must stay inside the pack.
Trusted local scripts can use absolute paths and `~/` paths.

Each image is limited to 16 MiB encoded, 4096 pixels per side and four megapixels.
Image transitions change at completion. Numeric and color transitions interpolate.

## Color and alpha

RGBA16F stores arbitrary buffer data. RGBA8 clamps buffer data.
Neither format enables display HDR.
Terminal input and final output use encoded sRGB with premultiplied alpha.
The host does not convert transfer functions automatically.

Use `srgbToLinear` and `linearToSrgb` for straight RGB.
Use `toLinearPremultiplied` and `toSrgbPremultiplied` for colors with alpha.
Transparent colors convert to zero to prevent division by zero.
The feedback-glow example stores linear premultiplied light and converts back during composition.

## Ownership and reset

Each effect occurrence, scope and pane owns two alternating textures per buffer.
Depth adds two auxiliary textures. No texture is sampled while it is being written.

These changes reset history:

- Layout or active-tab identity.
- Framebuffer size.
- Cell size or DPI.
- Theme foreground or background.
- Opacity.
- Shader reload.
- Clock rewind.

Live parameter updates retain history.
Focus changes can reset window-scoped buffers because their terminal context changes.
Chain removal, window destruction or GPU resource release destroys the buffers.

## Memory limits

| Storage | Limit and behavior |
| --- | --- |
| Persistent buffers | 256 MiB per window and 32 effect occurrences. Excess allocations use transparent inputs and log a warning. |
| Region input copies and uploaded images | A separate 256 MiB GPU cache. Excess inputs become transparent and log a warning. |
| Decoded live images | 256 MiB per window, including transition endpoints. Excess updates are rejected atomically. Shared images count once. |

Blocked buffers recover when their requested allocation fits the limit.
Unused GPU cache entries expire after one idle frame.
These limits count resident pixels.
Driver allocations, frames in flight, render targets, terminal textures, uniforms and pipelines require additional memory.

Each buffer uses:

```text
2 × rounded(width × scale) × rounded(height × scale) × bytesPerPixel
```

Auxiliary depth doubles this amount. Add the amounts for all panes and passes.
The feedback-glow half-resolution RGBA16F pair uses approximately 31.6 MiB at 4K or 7.9 MiB at 1080p.

## Examples

- [Feedback glow](../data/shaders/feedback-glow/README.md): one buffer with retained bright pixels.
- [Depth trails](../data/shaders/depth-trails/README.md): two buffers, depth, a vertex stage and an image parameter.
- [Phosphor](../data/examples/phosphor/README.md): a complete reactive scene.
