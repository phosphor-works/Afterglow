<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Shader pass API

Afterglow uses Phosphor ShaderRegistry metadata, source containment, include resolution, parameter slots and presets.
Phosphor ShaderNodeRhi renders Qt Quick items.
Afterglow's PostChain adapts the shared registry to terminal QRhi composition passes.
Afterglow reads the `geometryPasses` extension from pack metadata.
Qt Quick shaders with a different uniform block require adaptation to the terminal contract.

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
| `vertexShader` | Path to a terminal vertex hook. |
| `geometryPasses` | One mesh object or `null` per buffer shader. |

## Pass order and coordinates

`iChannel0` contains the current terminal or layer input in every pass.
`iBuffer0` through `iBuffer7` expose the configured buffers. `iBuffer` aliases `iBuffer0`.
Passes execute in metadata order.
Earlier buffers contain the current frame's result. The current and later buffers contain the previous frame.
Without feedback, previous values are cleared each frame.
The final image pass receives all current-frame buffers.

Input textures are local to the region.
The texture wrapper reverses Y to match `mainImage` coordinates.
Hardware wrapping and filtering also work through user-defined sampler helpers.
Raw `texelFetch` addresses local texture pixels.

Buffer passes use scaled buffer pixels for `iResolution`, region, cell, cursor and mouse coordinates.
The image pass uses full-resolution pixels.
Buffer `iFrame` starts at zero after reset. Image `iFrame` retains the window frame counter.
`iTime` is the shared host clock.

## Auxiliary depth

With depth enabled, write `depthBuffer = vec4(...)` in `mainImage`.
`iDepth`, also named `uDepthBuffer`, samples the preceding pass's auxiliary output.
The first buffer samples its own previous depth. The image pass samples the final buffer's depth.
Initial depth values are zero.
This color attachment stores arbitrary shader data.
Mesh passes use a separate hardware depth attachment for visibility tests.
The two attachments have different purposes.

## Vertex hook

A vertex file defines:

```glsl
void mainVertex(inout vec4 position, in vec2 uv)
```

Position starts in clip space from -1 to 1. UV starts at the top-left and ranges from 0 to 1.
The hook can use terminal uniforms, parameters and samplers.
After the hook, the host maps position into the current pane. Scissoring still isolates panes.
Buffer passes use fullscreen geometry unless `geometryPasses` selects a mesh.
The `mainVertex` hook applies only to the final image stage.

## Mesh passes

A buffer pass can draw triangle meshes with hardware depth testing and depth writes.
The final image pass samples the result through `iBuffer0` to `iBuffer7`.
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

A mesh vertex shader defines `void mainMesh(out vec4 clipPosition)`.
Imported meshes provide `meshPosition` (`vec3`), `meshNormal` (`vec3`) and `meshUV` (`vec2`).
Positions are in model coordinates. The shader applies model, camera and projection transforms.
The output uses Vulkan clip coordinates: X and Y from -W to W, and Z from 0 to W.
Positive Y points down. Smaller depth values are closer to the camera.
The rasterizer clips triangles and interpolates attributes with perspective correction.

Declare matching `layout(location=N) out` and `in` values in vertex and fragment shaders.
The fragment shader keeps the `mainImage` interface.
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
Depth comparison uses strict `less`; surfaces at equal depth keep the first fragment.
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

Up to four `image` or `texture` parameters use `uTexture0` through `uTexture3`.
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
- [Depth trails](../data/shaders/depth-trails/README.md): two buffers, depth, a vertex hook and an image parameter.
- [Phosphor](../data/examples/phosphor/README.md): a complete reactive scene.
