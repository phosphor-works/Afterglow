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

Supported metadata: `multipass: true`, one entry in `bufferShaders`,
`bufferFeedback`, `bufferScale` (0.125–1), `halfFloatBuffers`, clamp wrapping and
linear/nearest filtering (including their first per-buffer override). Additional
buffers, repeat/mirror wrap, custom vertex shaders and depth passes are rejected
with a diagnostic. This is not binary/GLSL compatibility with Phosphor's Qt Quick
uniform block. Both passes use Afterglow's `mainImage` and terminal uniforms.

- `iChannel0`: the current terminal/layer input in both passes.
- `iBuffer`: previous buffer in the buffer pass; newly written buffer in the
  image pass. It is transparent at initialization/reset. With feedback disabled,
  it is cleared every frame. The image pass still receives this frame's buffer.
- `iResolution`, region, cell, cursor and mouse coordinates in the buffer pass
  use scaled buffer pixels. The image pass uses full-resolution pixels.
- Buffer `iFrame` starts at zero after reset. The image pass retains the window
  frame counter. `iTime` is the shared host clock in both passes.
- The texture wrapper clamps sampling to the current pane, including half-texel
  margins. Raw `texelFetch` bypasses the wrapper and is the author's responsibility.

Buffer data is arbitrary RGBA16F by default, or clamped RGBA8 when
`halfFloatBuffers: false`. Neither format implies display HDR. Terminal input
and final output use encoded sRGB with premultiplied alpha. There is no automatic
transfer-function conversion. Use `srgbToLinear`/`linearToSrgb` for straight RGB,
or `toLinearPremultiplied`/`toSrgbPremultiplied` for colors with alpha. Transparent
colors convert to zero, preventing division by zero. This effect stores linear
premultiplied light and converts back only when compositing.

Each effect occurrence and scope owns two ping-pong textures per window. Panes
share scissored regions of those textures. No texture is sampled while being
written. Layout/active-tab identity, framebuffer size, cell size/DPI, theme
foreground/background, opacity, shader reload and clock rewind reset history.
Changing a live parameter deliberately retains history. Changing focus may reset
a window-scoped buffer because its terminal context changes. Disabling the
chain, window destruction or GPU resource release destroys all buffer resources.

Persistent buffers have a 256 MiB per-window pixel budget and a limit of 32
effect occurrences. An over-budget effect receives a transparent 1×1 fallback
and logs a warning; up to 32 fallback pairs plus one shared overflow pair add at
most 528 bytes of pixels. Driver allocations, render targets, terminal textures,
uniforms and pipelines are additional. At 4K this pack's half-resolution RGBA16F
pair consumes about 31.6 MiB; at 1080p, 7.9 MiB. The general formula is
`2 × rounded(width × scale) × rounded(height × scale) × bytesPerPixel`.
