<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Luau scripting

Afterglow uses Luau API version 1.
The [typed declarations](../afterglow/src/scripting/afterglow.d.luau) define helper signatures, events, snapshots and results.
Installed declarations are in `share/afterglow/api/afterglow.d.luau`.
Installed examples are in `share/afterglow/examples/scripts`.

## Install a script

Copy a `.luau` file into `scripts/` beside the active configuration file.
The usual directory is `~/.config/afterglow/scripts/`.
A script can handle terminal events and add command-palette actions.
Use `script:<id>.<action>` in a keybinding to run a script action.
Set `Scripts.enabled=false` to disable scripting.
See [packages](PACKAGES.md) for modules, dependencies and saved state.

The VM has no direct file or process API.
Installed scripts are trusted local code.
They can request file reads and process launches through host effects.
The VM sandbox does not prevent those explicit operations.

## Wait for results

`afterglow.await(effect)` applies an effect and waits for its result in the same handler.
It returns the result data. On failure, it returns `nil` and the error.
The third value is the complete result event.

```lua
function script.run_action(action, ev)
    if not ev.pane then return nil end
    local name = afterglow.await(afterglow.prompt(ev.pane, "Branch name"))
    if not name then return nil end -- the dialog was cancelled
    local switched, err = afterglow.await(afterglow.exec("switch", {"git", "switch", "-c", name.text}, {cwd = ev.pane.cwd}))
    if not switched then return afterglow.notify("Branch", err.message, ev.pane) end
    return afterglow.set_title(ev.pane, name.text)
end
```

Local variables keep their values across each wait.
The handler returns its effects when it finishes, as a handler without waits does.
Other handlers run while a handler waits.
`ev.terminal` keeps the snapshot from the handler's start.
The result event contains a new `terminal` snapshot.

`afterglow.await_all(effects)` applies up to 16 effects in order and waits for all of them.
It returns their result events in the same order. Each event has `ok`, `data` and `error`.
Use it to run processes or queries together.

```lua
local results = afterglow.await_all({
    afterglow.exec("branch", {"git", "branch", "--show-current"}, {cwd = ev.pane.cwd}),
    afterglow.exec("status", {"git", "status", "--short"}, {cwd = ev.pane.cwd}),
})
if results[1].ok then branch = results[1].data.stdout end
```

`afterglow.sleep(ms)` waits from 0 to 600,000 milliseconds.

Every handler can wait, except `migrate` and `on_unload`.
Module code that runs at load time cannot wait.
A wait inside a coroutine that the script creates is an error.

Each part of a handler between waits has the time limit of the handler type.
An event handler has 20 ms for each part. A user action has 100 ms for each part.
A script can have 16 waiting handlers and 16 sleeps.
Another wait returns the error code `busy`.
An error after a wait counts as a handler failure.
Awaited results count toward the result limits.
Replacement, removal or disable discards waiting handlers. They do not continue.

The [shell-region picker](../data/examples/scripts/semantic-regions.luau) waits for three results in one action.
The [project navigator](../data/examples/packages/project-navigator/init.luau) runs its project searches together with `await_all`.

## Effects and results

Helpers create effects. Return an effect or a list of effects from a handler to apply them.
Use `afterglow.await` when the handler needs the result.
`afterglow.request(id, effect)` requests an `on_result` callback instead.
Request IDs must contain 1–64 characters.
Use different IDs for requests that require separate tracking.

```lua
function script.run_action(action, ev)
    if ev.pane then
        return afterglow.request("workspace", afterglow.split(ev.pane, "right"))
    end
end

function script.on_result(ev)
    if ev.request_id ~= "workspace" then return nil end
    if not ev.ok then return afterglow.log(ev.error.message) end
    return afterglow.set_title(ev.data.pane, "Workspace")
end
```

| Result field | Meaning |
| --- | --- |
| `request_id` | Caller-supplied request ID |
| `kind` | Operation type |
| `ok` | Success flag |
| `data` | Operation-specific values |
| `terminal` | New terminal snapshot |
| `pane` | Initiating event's pane, when available |
| `error` | Error `code` and `message`, on failure |

Common errors are `invalid_effect`, `no_pane`, `rate_limited`, `operation_failed` and `unavailable`.
Direct creation effects return `data.pane`, `pane_id`, `tab_id` and `window_id`.
Other effects return an empty data table unless their reference specifies other fields.

Afterglow queues results for the originating script instance after it applies the operation.
The result records the operation at application time.
Its snapshot can already show that a new pane has closed.
Later effects validate the ID again.

Success confirms operation acceptance or application.
It does not confirm process completion or acceptance of an asynchronous dialog from a generic action.
Use direct creation helpers when you need a returned pane ID.

Replacement, removal or disable cancels outstanding results.
An unchanged script keeps its VM and pending results during reload.
Effect lists execute in order. They are not transactions.
Malformed request IDs receive no result.

## Execution limits

| Limit | Value |
| --- | --- |
| VM memory | 16 MiB |
| Event handler | 20 ms |
| User action | 100 ms |
| Effects in one returned list | 64 |
| Applied effects | 20 each second |
| Structural effects | 3 each 10 seconds |
| Queued results | 64 |
| Result delivery | 64 each second |
| Waiting handlers | 16 |
| Effects in one `await_all` | 16 |
| Sleeps | 16, each up to 600,000 ms |

A list with more than 64 effects applies nothing and causes an error.
Exceeding a result limit disables the script until its source changes.
Refused operations also count toward result limits.
Structural effects create or close panes, tabs or windows.
Generic actions and direct effects use the same structural budget.

## Terminal state

Each handler receives `ev.terminal`, a version 1 metadata snapshot.
It contains `windows`, `tabs`, `panes`, `active_window`, `active_tab` and `active_pane`.
It does not contain terminal text.
Changing the snapshot does not change the terminal.
Afterglow captures the snapshot before dispatch.
A queued event can describe an earlier command than the snapshot.
`ev.state` in `on_status` remains the program status string.

| Record | Fields |
| --- | --- |
| Window | `id`, `focused`, `active_tab`, `active_pane`, `project`, `project_directory`, `overview` |
| Tab | `id`, `window`, `index`, `title`, `active`, `active_pane` |
| Pane | `id`, `window`, `tab_id`, `tab`, `title`, `cwd`, `command`, `cols`, `rows`, `focused`, `context` |

A tab is active when its window selects it.
Each unfocused window keeps its selected tab and pane.
Top-level active IDs are zero when no Afterglow window has keyboard focus.
Pane, tab and window IDs exist only during the process lifetime.
`tab_id` remains stable during tab reordering.
`pane.tab` and the D-Bus tab field are zero-based positions.

Closed-pane events retain `id`, `tab_id` and `window`.
The closed pane is absent from the live snapshot.
Use these IDs to update surviving panes in the affected tab.

`afterglow.panes(state, scope, anchor)` filters an existing snapshot.
Scope is `all`, `pane`, `tab` or `window`. The default is `all`.
The anchor can be a pane record or ID.
Without an anchor, the helper uses the globally active objects.
A missing anchor ID gives no matches.

```lua
function script.run_action(action, ev)
    local panes = afterglow.panes(ev.terminal, "tab", ev.pane)
    return afterglow.log("This tab has " .. #panes .. " panes")
end
```

## Events and lifecycle

Pane events include `on_pane_created`, `on_pane_closed`, `on_pane_moved`, `on_focus`, `on_bell` and `on_title`.
Other events are `on_status`, `on_command_started`, `on_command_finished`, `on_user_var`, `on_context` and `on_tab_switch`.
These events contain the applicable pane and terminal snapshot.

A split's `on_pane_created` also contains `split_from`, the pane it split from, and `edge`.
`edge` is the side the two panes share: 1 left, 2 top, 3 right or 4 bottom.
`on_tab_switch` contains `from_tab`, `to_tab` and `direction`, 1 toward a tab on the right and -1 toward one on the left.
Its pane is the new tab's focused pane.

`run_action(action, ev)` handles declared palette and key actions.
`on_load`, `on_unload` and `on_config_changed` handle lifecycle changes.
`on_timer` and `on_file_changed` contain the task name.

A broken replacement keeps the last working instance.
Successful replacement, removal, disable or repeated handler errors cancel tasks and remove UI contributions.
`on_unload` uses the normal handler time limit. Its returned effects are ignored.

## Tasks

Task names contain 1–64 characters and belong to one script instance.
Limits are 16 active tasks for each script and 128 in total.
Reusing an active name returns `busy`.
`afterglow.cancel(name)` cancels the owned timer, watcher or job.

| Effect | Behavior |
| --- | --- |
| `timer(name, delay_ms, repeat_ms?)` | Calls `on_timer` with `ev.name` |
| `exec(name, argv, options?)` | Starts an asynchronous process and returns a result with the task name |
| `read_file(name, path, options?)` | Reads a regular file asynchronously and returns `text` and `truncated` |
| `watch_file(name, path)` | Monitors an existing regular file and calls `on_file_changed` |

Omit `repeat_ms` for a single timer callback.
A repeating timer has an interval of at least 250 ms.
The interval starts after its handler returns. Callbacks do not overlap.
Timer and watcher delivery is limited to 64 callbacks each second for each instance.
Watcher events contain `name`, `path` and `exists`.
Watchers handle atomic file replacement and combine changes within 250 ms.

Process options include `cwd`, `env`, `timeout_ms` and `max_bytes`.
File reads accept the timeout and byte-limit options.
Timeout defaults to 30,000 ms and cannot exceed 600,000 ms.
The output limit defaults to 65,536 bytes and cannot exceed 1 MiB.
At most eight file workers run together. Special files are not permitted.

Normal process results contain `stdout`, `stderr`, `exit_code` and `truncated`.
Errors include `spawn_failed`, `process_failed`, `timeout`, `output_limit`, `cancelled` and `io_error`.
Exceeding the combined stdout/stderr limit stops the job and returns partial output.
Timeout, cancellation and unload terminate the process group.
Afterglow discards callbacks for retired instances.

## Native UI

Wrap `select(pane, title, items)` or `prompt(pane, title, initial_text?)` with `afterglow.request`.
Selectors accept up to 500 unique `{id, title}` items and provide fuzzy filtering.
Selection results contain `id` and `title`. Prompt results contain `text`.
Escape, pane closure or window closure cancels a dialog.
A second dialog in the same window returns `busy`.
Dialogs use the desktop Qt Quick Controls theme.

`set_actions` replaces up to 32 palette actions for a script with a `run_action` handler.
`status(pane, text?, tab_title?)` replaces the status label and tab suffix.
Empty strings clear these contributions.

`override(pane, values)` replaces the owner's pane appearance settings.
Supported fields are `cursorBlink`, `backgroundOpacity` and `unfocusedDim`.
The last two fields accept values from zero to one.
The latest script value for each field takes priority over configuration.
Removal exposes the next owner's value or the configured value.
An empty table clears the owner's values.
Unload and pane closure remove all contributions.

## Key modes

`key_mode(pane, label?)` sends the keys of the pane's window to the script's `on_key` handler.
The keys do not reach the shell. The window shows the label.
A script needs an `on_key` handler to start a key mode.
One script at a time holds a window's keyboard. Another script's request returns `busy`.

`on_key` receives `key`, `text`, `ctrl`, `shift`, `alt`, `meta` and `repeat`.
`key` is the typed character, such as `a` or `A`, or a name: `Enter`, `Escape`, `Tab`, `Backspace`, `Delete`, `Insert`, `Home`, `End`, `PageUp`, `PageDown`, `Space`, `Left`, `Right`, `Up`, `Down` or `F1` to `F35`.
With Ctrl, `key` is the letter, and `text` is empty.
`end_key_mode()` gives the keyboard back.
A handler error, disable, replacement or removal of the script also ends its key mode.
Key releases are not reported.

## Pane overview

`overview(pane, operation)` drives the pane overview of the pane's window.
`open` opens it from the pane. `close` closes it and keeps the focus. `choose` focuses the highlighted pane.
`overview_move(pane, step)` moves the highlight by `step` cards. `overview_move(pane, 0, target)` highlights the pane `target`.
Results contain the overview: `open`, `highlighted` and `cards`.
The snapshot's window records contain the same `overview` table. `cards` lists pane IDs in tab order while the overview shows.

Combine these effects with a key mode to replace the overview's keys.
The [scripted overview package](../data/examples/packages/scripted-overview/init.luau) moves with a, d, w and s, and ships its own overview shader.
A click still closes the overview, so check `overview.open` in `on_key` and end the key mode when it is false.

## Pane pulses

`pulse(pane, effect, options?)` plays an effect over the pane in `Shaders.paneEffects`.
The bundled Pane Effects pack draws `portal`, a ring that sweeps out; `flash`, a light that fades; and `shake`.
Numbers from 4 to 255 select effects that other packs define.
`options.duration_ms` sets the time, from 50 to 10,000 ms. The default is 600 ms.
`options.color` is a palette index from 0 to 255. The default is 4.
A new pulse on the same pane replaces the old one.
The [pulse example](../data/examples/scripts/pane-pulses.luau) flashes failed commands and shakes on the bell.

## Launch options

`split`, `new_tab` and `new_window` accept a final `LaunchOptions` table.
`cwd` sets the working directory.
`env` changes inherited environment variables.
`size` sets the new split fraction, greater than zero and less than one.
Split directions are `left`, `right`, `up` and `down`.
Failed launches return errors and remove the incomplete ordinary pane.

Set an environment value to `false` to remove it.
Other environment values must be strings.
`exec` accepts the same `env` map.
Each map permits 256 variables and 64 KiB of combined names and values.

```luau
return afterglow.split(ev.pane, "left", {"fish"}, {
    cwd = "/tmp", size = 0.3,
    env = {PROJECT_MODE = "development", INHERITED_SECRET = false},
})
```

## Text and semantic regions

`read_text` accepts `screen`, `scrollback`, `last-output` and `selection`.
The default scope is `last-output`.
Results contain `text`, `available` and `truncated`.
`max_bytes` limits UTF-8 output to 1–1048576 bytes. The default is 65536 bytes.
A missing selection or unavailable region returns `available=false`.

`semantic_regions` returns up to 1024 retained shell regions in chronological order.
Each region has these fields:

| Field | Meaning |
| --- | --- |
| `id` | Stable string ID |
| `kind` | `prompt`, `input` or `output` |
| `start`, `finish` | Start position and exclusive end position |
| `open` | Region is still receiving content |
| `available` | Region still has retained text |
| `start_trimmed` | History removal deleted the region start |
| `exit_code` | Optional completed-output exit status |

Positions use stable row IDs encoded as strings and zero-based columns.
Strings avoid Luau number precision loss.
`read_region(pane, id, max_bytes)` returns logical text for that region.
Unknown or fully discarded IDs are unavailable.
Reset does not reuse IDs. Retained positions follow text reflow.

Set `Scripts.allowReadText=false` to deny these queries.
This setting is independent of external `Control.allowReadText` permissions.
It controls terminal queries, not operating-system file or process access.

The [shell-region picker](../data/examples/scripts/semantic-regions.luau) copies a selected region, with a 256 KiB limit.
The [build inspector](../data/examples/scripts/build-inspector.luau) finds compiler locations in command output.
The [creation example](../data/examples/scripts/create-and-title.luau) uses launch options and a returned pane ID.

## Diagnostics

`afterglow ctl scripts` reports `calls`, `handler_errors`, `effect_errors` and `last_error`.
Timing fields are `last_call_ms`, `max_call_ms` and `total_call_ms`.
Diagnostics also count dropped callbacks, effects and results.
`waiting` is the number of handlers that wait for a result.
Timing measures VM handler execution, not asynchronous process duration.
Deferred failures count when the operation completes.
Unchanged instances retain counters. Replacements start new counters.

## Helpers


| Helper | Signature |
| --- | --- |
| `afterglow.panes` | `(state: State, scope: ("all" \| "pane" \| "tab" \| "window")?, anchor: PaneRef?) -> { Pane }` |
| `afterglow.await` | `(effect: Effect) -> (any?, OperationError?, ResultEvent)` |
| `afterglow.await_all` | `(effects: { Effect }) -> { ResultEvent }` |
| `afterglow.sleep` | `(ms: number) -> (any?, OperationError?, ResultEvent)` |
| `afterglow.request` | `(id: string, effect: Effect) -> Effect` |
| `afterglow.key_mode` | `(pane: PaneRef, label: string?) -> Effect` |
| `afterglow.end_key_mode` | `() -> Effect` |
| `afterglow.overview` | `(pane: PaneRef, operation: ("open" \| "close" \| "choose")?) -> Effect` |
| `afterglow.overview_move` | `(pane: PaneRef, step: number?, target: PaneRef?) -> Effect` |
| `afterglow.pulse` | `(pane: PaneRef, effect: string, options: { duration_ms: number?, color: number? }?) -> Effect` |
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


## Related references

- [Packages and saved state](PACKAGES.md)
- [Project operations](PROJECTS.md)
- [Shader signals, parameters and presets](SHADERS.md#script-controls)
