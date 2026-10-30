<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Migration notes

## D1 desktop and compatibility update

No configuration schema or Luau API version bump is required. Existing settings, scripts and package state retain their formats. The new `--config-reference` command is read-only.

Behavior changes:

- OSC 1337 files without `inline=1`, previously ignored, now offer an explicit Save dialog. Inline images retain their existing behavior.
- File drops insert quoted paths into the pane under the pointer. No newline is appended.
- IMEs receive cursor-row context and no longer receive a Latin preference hint. Surrounding-text replacement is still unavailable.
- Kitty relative placement keys are now interpreted; invalid parents and cycles receive protocol errors. Applications should use `q=2` when they do not consume replies.
- Extra cursor support is discoverable with `CSI > SPACE q`. Clients must use the returned operation list rather than assume optional color operations.

## Earlier delivery milestones

`Terminal.copyOnSelect` accepts `primary`, `clipboard` and `off`; legacy booleans map to `primary` and `off`. Prefer the named values in new configs.

Luau API version 1 uses stable pane/tab/window IDs, correlated operation results and returned effects. Call `afterglow.request(id, effect)` and handle `on_result` for operations with results; do not assume an effect has already completed when it is created. Shader instance IDs come from `afterglow.shaders`, and are distinct from pack IDs. Reset live overrides with an empty parameter table.

Package manifests declare `api_version`, `state_version`, entry module and module map. Durable state changes require a package `migrate(value, oldVersion, newVersion)` function when the stored state version changes. State writes use revision checks. See the [package lifecycle reference](../README.md#luau-packages-and-persistent-state) and [typed API](../afterglow/src/scripting/afterglow.d.luau).
