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
| `afterglow.read_text` | `(pane: PaneRef, scope: ("screen" \| "scrollback" \| "last-output")?, max_bytes: number?) -> Effect` |
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
| `afterglow.exec` | `(name: string, command: { string }, options: { cwd: string?, timeout_ms: number?, max_bytes: number? }?) -> Effect` |
| `afterglow.read_file` | `(name: string, path: string, options: { timeout_ms: number?, max_bytes: number? }?) -> Effect` |
| `afterglow.watch_file` | `(name: string, path: string) -> Effect` |
| `afterglow.set_actions` | `(actions: { { id: string, title: string } }) -> Effect` |
| `afterglow.select` | `(pane: PaneRef, title: string, items: { { id: string, title: string } }) -> Effect` |
| `afterglow.prompt` | `(pane: PaneRef, title: string, text: string?) -> Effect` |
| `afterglow.status` | `(pane: PaneRef, text: string?, tab_title: string?) -> Effect` |
| `afterglow.override` | `(pane: PaneRef, values: { cursorBlink: boolean?, backgroundOpacity: number?, unfocusedDim: number? }) -> Effect` |
| `afterglow.send_text` | `(pane: PaneRef, text: string) -> Effect` |
| `afterglow.notify` | `(title: string, body: string?, pane: PaneRef?) -> Effect` |
| `afterglow.split` | `(pane: PaneRef, direction: ("right" \| "down")?, command: (string \| { string })?) -> Effect` |
| `afterglow.new_tab` | `(pane: PaneRef, cwd: string?, command: (string \| { string })?) -> Effect` |
| `afterglow.new_window` | `(cwd: string?, command: (string \| { string })?) -> Effect` |
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

`on_pane_created`, `on_pane_closed`, `on_pane_moved`, `on_focus`, `on_bell`, `on_title`, `on_status`, `on_command_started`, `on_command_finished`, `on_user_var` and `on_context` carry the relevant pane and terminal snapshot. Closed-pane events retain only the closed ID.

`run_action(action, ev)` handles declared palette/key actions. `on_load`, `on_unload` and `on_config_changed` manage lifecycle. `on_timer` and `on_file_changed` carry the task name. `on_result` carries request ID, success, data or structured error and a fresh terminal snapshot.

Packages receive package paths/config/assets plus stored value, schema version and revision in `on_load`. `migrate(value, oldVersion, newVersion)` returns the migrated object. Reload cancels old tasks and rejects stale completions. Do not use `io`, unrestricted filesystem access, network APIs or undeclared modules; return bounded host effects instead.

## Examples

| Workflow | Package |
| --- | --- |
| Projects and native selection | [Project navigator](../data/examples/packages/project-navigator) |
| Scene parameters and presets | [Scene controls](../data/examples/packages/scene-controls) |
| Context-aware scene intensity | [Contextual scene](../data/examples/packages/contextual-scene) |

Browse the [full package catalog](../data/examples/packages) for the remaining examples. Each package README describes installation, actions and required options. Start with its actual manifest and modules rather than copying only an entry file.
