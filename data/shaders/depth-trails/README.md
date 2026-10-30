# Depth trails

Add `"depth-trails"` to `Shaders.background` for a small executable reference of
all terminal shader stages. This is an API example; the Phosphor scene remains
the polished demonstration.

The first buffer generates a moving field and auxiliary depth. The second reads
that field and depth, blends its own previous frame, and forwards depth. The
image pass lights the field using `iDepth`. A custom vertex hook gently pulses
an oversized quad. Mirror sampling on the first buffer keeps its shifted edge
continuous without sampling another pane.

Use **Edit scene parameters and presets** to change intensity or enter an
optional image path. Paths can be pack-relative, absolute, or start with `~/`;
an empty path restores a transparent image. Live image changes take effect at
transition completion and can be saved in presets. Each image is bounded to
16 MiB encoded, 4096 pixels per side and four megapixels decoded.

At 1080p, two half-resolution RGBA16F passes with feedback and auxiliary depth
consume about 31.6 MiB; at 4K they consume about 126.6 MiB. Remove the pack to
release its buffers and stop animation. See the [buffer contract](../feedback-glow/README.md)
for pass ordering, coordinate conventions, reset rules and global budgets.
