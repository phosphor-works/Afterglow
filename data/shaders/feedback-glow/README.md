# Feedback glow

Add `"feedback-glow"` to `Shaders.pane` (or `Shaders.text` for glyph-only
retention). Enable the scene-controls package to select Subtle/Lingering, edit
intensity/decay, save a preset or reset its overrides. Removing the shader from
configuration releases its buffers. This is a small reference effect for the
persistent-buffer contract, not the full Phosphor scene.

The buffer accumulates bright pixels in linear light at half resolution. The
image pass adds a restrained afterimage without replacing current glyphs.
`strength: 0` restores the input visually; remove the shader to stop its frame
loop and release GPU resources. Reducing intensity does not reduce buffer cost.

## Terminal buffer contract

Afterglow reuses Phosphor ShaderRegistry metadata, source containment, include
resolution, parameter slots and presets. Phosphor's ShaderNodeRhi renders Qt
Quick items and cannot directly own a terminal window's QRhi composition passes.
PostChain supplies that missing adapter; no second pack parser was introduced.

Supported metadata: `multipass: true`, up to eight entries in `bufferShaders`,
`bufferFeedback`, `bufferScale` (0.125–1), `halfFloatBuffers`,
`bufferWrap`/`bufferWraps` (clamp/repeat/mirror), and
`bufferFilter`/`bufferFilters` (linear/nearest). `depthBuffer: true` adds an
auxiliary color attachment to every buffer pass. `vertexShader` names a custom
terminal vertex hook. This uses Afterglow's terminal uniform contract; Qt Quick
shaders with a different uniform block need porting.

- `iChannel0` is the current terminal/layer input in every pass.
- `iBuffer0` through `iBuffer7` expose the configured buffers. `iBuffer` aliases
  `iBuffer0`. Passes execute in metadata order: earlier buffers contain this
  frame's result; the current and later buffers contain their previous frame.
  With feedback disabled, previous values are cleared each frame. The final
  image pass receives all of this frame's buffers.
- All inputs are region-local textures. The texture wrapper flips Y to match
  `mainImage` coordinates; hardware wrapping/filtering works even through
  user-defined sampler helpers. Raw `texelFetch` addresses local texture pixels.
- `iResolution`, region, cell, cursor and mouse coordinates in a buffer pass use
  scaled buffer pixels. The image pass uses full-resolution pixels.
- Buffer `iFrame` starts at zero after reset. The image pass retains the window
  frame counter. `iTime` is the shared host clock.
- With depth enabled, write `depthBuffer = vec4(...)` in `mainImage`.
  `iDepth` (also `uDepthBuffer`) samples the preceding pass's auxiliary output;
  the first buffer samples its own previous depth, and the image pass samples
  the final buffer's depth. It starts at zero. This is arbitrary shader data,
  not hardware depth testing or a mesh scene graph.
- A vertex file defines `void mainVertex(inout vec4 position, in vec2 uv)`.
  Position begins in clip space (-1 to 1); UV uses a top-left origin (0 to 1).
  The hook can use the terminal uniforms, parameters and samplers. After the
  hook, position is mapped into the current pane. Scissoring still isolates
  panes. Buffer passes retain fullscreen geometry; the hook applies to the
  final image stage.
- Up to four `image`/`texture` parameters sample `uTexture0` through `uTexture3`.
  Defaults, live updates, presets and reset share the same path validation.
  Empty paths are transparent. Relative paths stay inside the pack, including
  symlinks; absolute paths and `~/` are supported for trusted local scripts.
  Images are at most 16 MiB encoded, 4096 pixels per side and four megapixels.
  Image transitions switch at completion; numeric/color transitions interpolate.

Buffer data is arbitrary RGBA16F by default, or clamped RGBA8 when
`halfFloatBuffers: false`. Neither format implies display HDR. Terminal input
and final output use encoded sRGB with premultiplied alpha. There is no automatic
transfer-function conversion. Use `srgbToLinear`/`linearToSrgb` for straight RGB,
or `toLinearPremultiplied`/`toSrgbPremultiplied` for colors with alpha. Transparent
colors convert to zero, preventing division by zero. This effect stores linear
premultiplied light and converts back only when compositing.

Each effect occurrence, scope and pane owns two ping-pong textures per buffer
(and two auxiliary textures when depth is enabled). No texture is sampled while
being written. Layout/active-tab identity, framebuffer size, cell size/DPI,
theme foreground/background, opacity, shader reload and clock rewind reset
history. Live parameter updates retain history. Changing focus can reset a
window-scoped buffer because its terminal context changes. Removing a chain,
destroying its window or releasing GPU resources destroys its buffers.

Persistent buffers share a 256 MiB per-window pixel budget and a limit of 32
effect occurrences. Over-budget buffers sample a transparent fallback and log
a warning, and can recover when their requested allocation fits. Region-local
input copies and uploaded images share a separate 256 MiB GPU cache budget;
oversize inputs become transparent with a warning. Unused cache entries expire
after one idle frame. Decoded live image overrides, including transition
endpoints, have a 256 MiB per-window budget; over-budget updates are rejected
atomically. Shared images are counted once.

These are resident pixel budgets. Driver allocations, in-flight frames, render
targets, terminal textures, uniforms and pipelines are additional. At 4K this
pack's half-resolution RGBA16F pair consumes about 31.6 MiB; at 1080p, 7.9 MiB.
Per buffer, the formula is
`2 × rounded(width × scale) × rounded(height × scale) × bytesPerPixel`;
auxiliary depth doubles it. Sum across panes and passes.
