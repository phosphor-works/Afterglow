<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Release notes

## Unreleased

Target platform: Linux on Wayland with Vulkan.
Native desktop tests use KDE/KWin. Other compositors and screen readers are not individually certified.
Build and installation procedures do not change. This release does not add macOS or Windows support.

- Keep windows responsive when shader regions move outside the surface during resize or tab detachment. Close GPU frames on rendering errors and release chrome resources in the correct order.
- Render imported OBJ meshes and shader-generated triangles with instancing and hardware depth testing. Add the Mesh Reactor shader pack.
- Detach a tab by dragging it outside the bar, or through its context menu. Existing panes keep their running processes.
- Reuse history rows during resize when their text still fits on one line. Preserve their cells, styles and graphemes without rebuilding them.
- Limit the command palette to eight visible results. Keep text clear of the scrollbar and show a result count and keyboard hints.
- Expose each current-tab terminal pane through Qt/AT-SPI, including viewport text, cursor, selection, geometry and change events.
- Provide read-only IME surrounding text for the live cursor row and remove the Latin-only preference hint.
- Support Kitty file transfers with approval, directories, links, compression and delta updates.
- Support Kitty drag-and-drop offers and receivers, including remote directory data and thumbnails.
- Complete Kitty color-control rendering and separate main/alternate-screen pointer-shape stacks.
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


Width changes still reflow wrapped lines and lines that exceed the new width.
See [performance measurements](docs/PERFORMANCE.md) for timings, workloads and limits.

See [compatibility](docs/COMPATIBILITY.md) for protocol limits and memory costs.
See [desktop integration](docs/DESKTOP.md) for accessibility and input-method limits.
See [bidirectional text](docs/BIDI.md) for the application-grid policy.

[Testing](docs/TESTING.md) describes regression and native desktop test procedures.
[Migration](docs/MIGRATION.md) describes behavior changes.
