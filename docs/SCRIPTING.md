<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Luau API reference

API version 1. The [typed declaration file](../afterglow/src/scripting/afterglow.d.luau) defines every event, snapshot, result and helper signature; it is the source of truth for editor integration. The table below indexes all helpers.

Helpers construct effects. Return an effect or a list of effects from a handler to execute them. `panes` filters an existing snapshot locally; `request` wraps an effect so its completion returns to the originating script instance through `on_result`. A snapshot is immutable event data, not a live object. Use IDs to target panes, tabs and windows; closed IDs fail through operation results.

See [state and identity](../README.md#terminal-state-in-luau), [operation results](../README.md#operation-results), [async tasks and native UI](../README.md#script-lifecycle-tasks-and-native-ui), [package lifecycle and storage](../README.md#luau-packages-and-persistent-state), and [shader controls](../README.md#live-named-shader-parameters) for payloads, limits and examples.

## Helpers

| Helper | Signature |
| --- | --- |
| `afterglow.panes` | `(state: State, scope: ("all" \| "pane" \| "tab" \| "window")?, anchor: PaneRef?) -> { Pane }` |
| `afterglow.request` | `(id: string, effect: Effect) -> Effect` |
| `afterglow.semantic_regions` | `(pane: PaneRef) -> Effect` |
| `afterglow.read_region` | `(pane: PaneRef, region: string, max_bytes: number?) -> Effect` |
| `afterglow.read_text` | `(pane: PaneRef, scope: ("screen" \| "scrollback" \| "last-output" \| "selection")?, max_bytes: number?) -> Effect` |
| `afterglow.state_read` | `() -> Effect` |
| `afterglow.state_write` | `(value: {[string]: any}, revision: number) -> Effect` |
| `afterglow.open_directory` | `(cwd: string, layout: string?) -> Effect` |
| `afterglow.save_project` | `(pane: PaneRef, name: string, overwrite: boolean?) -> Effect` |
| `afterglow.open_project` | `(name: string, newInstance: boolean?) -> Effect` |
| `afterglow.list_projects` | `() -> Effect` |
| `afterglow.move_pane` | `(pane: PaneRef, target: PaneRef?, placement: ("left" \| "right" \| "up" \| "down" \| "new-tab" \| "new-window")?) -> Effect` |
| `afterglow.detach_tab` | `(pane: PaneRef) -> Effect` |
| `afterglow.timer` | `(name: string, delay_ms: number, repeat_ms: number?) -> Effect` |
| `afterglow.cancel` | `(name: string) -> Effect` |
| `afterglow.exec` | `(name: string, command: { string }, options: { env: {[string]: string \| false}?, cwd: string?, timeout_ms: number?, max_bytes: number? }?) -> Effect` |
| `afterglow.read_file` | `(name: string, path: string, options: { timeout_ms: number?, max_bytes: number? }?) -> Effect` |
| `afterglow.watch_file` | `(name: string, path: string) -> Effect` |
| `afterglow.set_actions` | `(actions: { { id: string, title: string } }) -> Effect` |
| `afterglow.select` | `(pane: PaneRef, title: string, items: { { id: string, title: string } }) -> Effect` |
| `afterglow.prompt` | `(pane: PaneRef, title: string, text: string?) -> Effect` |
| `afterglow.status` | `(pane: PaneRef, text: string?, tab_title: string?) -> Effect` |
| `afterglow.override` | `(pane: PaneRef, values: { cursorBlink: boolean?, backgroundOpacity: number?, unfocusedDim: number? }) -> Effect` |
| `afterglow.send_text` | `(pane: PaneRef, text: string) -> Effect` |
| `afterglow.notify` | `(title: string, body: string?, pane: PaneRef?) -> Effect` |
| `afterglow.split` | `(pane: PaneRef, direction: ("right" \| "down" \| "left" \| "up")?, command: (string \| { string })?, options: LaunchOptions?) -> Effect` |
| `afterglow.new_tab` | `(pane: PaneRef, cwd: string?, command: (string \| { string })?, options: LaunchOptions?) -> Effect` |
| `afterglow.new_window` | `(cwd: string?, command: (string \| { string })?, options: LaunchOptions?) -> Effect` |
| `afterglow.focus` | `(pane: PaneRef) -> Effect` |
| `afterglow.close` | `(pane: PaneRef) -> Effect` |
| `afterglow.set_title` | `(pane: PaneRef, title: string) -> Effect` |
| `afterglow.copy` | `(text: string) -> Effect` |
| `afterglow.open_url` | `(url: string) -> Effect` |
| `afterglow.action` | `(pane: PaneRef, name: string) -> Effect` |
| `afterglow.shaders` | `(pane: PaneRef) -> Effect` |
| `afterglow.scene_state` | `(pane: PaneRef) -> Effect` |
| `afterglow.shader_presets` | `(pane: PaneRef, instance: string) -> Effect` |
| `afterglow.shader_preset` | `(pane: PaneRef, instance: string, preset: string, scope: ("pane" \| "tab" \| "window")?, transition: ShaderTransition?) -> Effect` |
| `afterglow.save_shader_preset` | `(pane: PaneRef, instance: string, name: string) -> Effect` |
| `afterglow.shader_parameters` | `(pane: PaneRef, instance: string, values: {[string]: number \| boolean \| string}, scope: ("pane" \| "tab" \| "window")?, transition: ShaderTransition?) -> Effect` |
| `afterglow.signal` | `(pane: PaneRef, channel: number, value: number?) -> Effect` |
| `afterglow.signals` | `(pane: PaneRef, values: { SignalValue }, scope: ("pane" \| "tab" \| "window")?) -> Effect` |
| `afterglow.log` | `(message: string) -> Effect` |

## Events

`on_pane_created`, `on_pane_closed`, `on_pane_moved`, `on_focus`, `on_bell`, `on_title`, `on_status`, `on_command_started`, `on_command_finished`, `on_user_var` and `on_context` carry the relevant pane and terminal snapshot. Closed-pane events retain the closed pane's `id`, `tab_id` and `window`.

`run_action(action, ev)` handles declared palette/key actions. `on_load`, `on_unload` and `on_config_changed` manage lifecycle. `on_timer` and `on_file_changed` carry the task name. `on_result` carries request ID, success, data or structured error and a fresh terminal snapshot.

Packages receive package paths/config/assets plus stored value, schema version and revision in `on_load`. `migrate(value, oldVersion, newVersion)` returns the migrated object. Reload cancels old tasks and rejects stale completions. Do not use `io`, unrestricted filesystem access, network APIs or undeclared modules; return bounded host effects instead.

## Examples

| Workflow | Package |
| --- | --- |
| Projects and native selection | [Project navigator](../data/examples/packages/project-navigator) |
| Scene parameters and presets | [Scene controls](../data/examples/packages/scene-controls) |
| Context-aware scene intensity | [Contextual scene](../data/examples/packages/contextual-scene) |

Browse the [full package catalog](../data/examples/packages) for the remaining examples. Each package README describes installation, actions and required options. Start with its actual manifest and modules rather than copying only an entry file.

## Launch options and child environments

`split`, `new_tab` and `new_window` accept a final `LaunchOptions` table. `cwd` selects the working directory, `env` merges environment variables into the inherited environment, and `size` sets the new split fraction between zero and one. Splits accept `left`, `right`, `up` and `down`. Set an environment value to `false` to remove it; other values must be strings. `exec` accepts the same `env` map in its options. Each map is limited to 256 variables and 64 KiB of combined names and values.

```luau
return afterglow.split(ev.pane, "left", {"fish"}, {
    cwd = "/tmp", size = 0.3,
    env = {PROJECT_MODE = "development", INHERITED_SECRET = false},
})
```

## Bounded text and semantic regions

`read_text` supports `screen`, `scrollback`, `last-output` and `selection`. Text results contain `text`, `available` and `truncated`; `max_bytes` is a UTF-8 byte limit between 1 and 1048576 (default 65536). A missing selection or unavailable retained region returns `available=false`.

`semantic_regions` returns up to 1024 retained shell-integration regions in chronological order. Each region has a stable string `id`, a `kind` (`prompt`, `input` or `output`), `start` and exclusive `finish` positions, `open`, `available` and `start_trimmed`. Completed output can carry `exit_code`. Positions use stable row IDs encoded as strings and zero-based columns, avoiding Luau number precision loss. `read_region(pane, id, max_bytes)` reads that region in logical text order. Unknown or fully discarded IDs are unavailable; IDs are not reused after reset. Retained positions follow reflow.

`Scripts.allowReadText=false` denies all of these queries. It is independent of `Control.allowReadText`, which governs external CLI/D-Bus reads. Scripts are local trusted code with separate file/process effects; this setting controls the terminal query API rather than providing an OS security sandbox.

## Diagnostics and lifecycle identity

Script diagnostics include total `calls`, `handler_errors`, `effect_errors`, `last_error`, `last_call_ms`, `max_call_ms`, `total_call_ms`, and dropped callback/effect/result counts. Call timing measures VM handler execution, excluding asynchronous process duration. Deferred operation failures are counted when they complete. Reloading an unchanged instance retains its counters; replacing it starts fresh counters.

`on_pane_closed` retains the closed pane's `id`, `tab_id` and `window` even after its live pane is gone. Use these identities to update surviving panes in the affected tab instead of falling back to whichever tab is currently active.

The [shell-region picker](../data/examples/scripts/semantic-regions.luau) lists
retained prompt/input/output regions and copies a selected region with a 256 KiB
limit. The [create-and-title example](../data/examples/scripts/create-and-title.luau)
uses split size, working directory and environment options before targeting the
returned pane identity. Copy either script into the active config's `scripts/`
directory and run its action from the command palette.
