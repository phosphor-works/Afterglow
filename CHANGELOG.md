<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Release notes

## Unreleased — D1 desktop and compatibility

Target platform: Linux on Wayland with a Vulkan driver. Native desktop acceptance is exercised on KDE/KWin; other compositors and screen readers are not individually certified. This update does not change the build/install workflow or establish macOS/Windows support.

- Expose each current-tab terminal pane through Qt/AT-SPI, including viewport text, cursor, selection, geometry and change events.
- Provide read-only IME surrounding text for the live cursor row and remove the Latin-only preference hint.
- Accept local file drops with shell quoting and bracketed paste into the target pane.
- Offer bounded OSC 1337 downloads through a native Save dialog, with atomic file writes.
- Support Kitty relative image placements, parent chains and Unicode placeholder anchors.
- Support queried multiple-cursor shapes and positions, with correct mixed block/beam layering and cursor shader processing.
- Add a user guide, complete schema-generated configuration reference, Luau API index, compatibility examples and a headless `--config-reference` command.

- Add opt-in primary-screen bidi layout with logical copy and mapped pointer, cursor, accessibility and IME geometry.
- Complete Luau launch options, bounded semantic-region/selection reads and lifecycle diagnostics.
- Support up to eight shader buffers, four image parameters, auxiliary depth output, custom vertex hooks and sampler modes; include executable depth-trails and semantic-region examples.
- Harden PTY startup/backpressure, parser resets and bounds, graphics decoding, script file reads, reload ownership and shader allocation recovery.
- Exercise native split creation, chrome keyboard routing, paste confirmation, IME commits and compositor-delivered drag/drop in an isolated KWin desktop.

Known limits: accessibility exposes the viewport rather than the entire history; rectangular selections report an encompassing range. IME replacement edits remain unsupported. Bidi is opt-in and preserves application-controlled grids; see [the bidi policy](docs/BIDI.md). Multiple-cursor custom text/fill colors are supported. Downloads are capped at 8 MiB and one pending request. Relative parent chains are capped at 32. Layered rendering uses one additional frame texture for mixed cursor order; see the [compatibility reference](docs/COMPATIBILITY.md) for memory costs and precise semantics.

Validation: [Regression and native desktop acceptance](docs/VALIDATION.md).

See [migration guidance](docs/MIGRATION.md) for behavior changes, [desktop workflows](docs/DESKTOP.md), and [protocol acceptance](docs/COMPATIBILITY.md). Packaging, clean-machine installation and package/build CI remain deferred at the user's request.
