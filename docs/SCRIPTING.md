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
A script can handle terminal events and add [commands](#commands).
Run a command with `aglow run <id> <command>`, from the command palette, from the context menu, with a key or with `afterglow ctl action`.
Set `Scripts.enabled=false` to disable scripting.
See [packages](PACKAGES.md) for modules, dependencies and saved state.

A script file can have at most 1 MiB. Afterglow does not load a larger file.
Files that end in `.d.luau` are type declarations. Afterglow does not load them.

The script ID is `metadata.id`, or the file name without `.luau`.
It has 1 to 64 letters, digits, `_` and `-`, and must not start with `-`.
A script with an ID that breaks this rule does not load. `afterglow ctl scripts` shows the error.
`metadata.name` is the name that `afterglow ctl scripts` shows. The default is the script ID.
Afterglow also reads `afterglow/scripts` in each system data directory, such as `/usr/share/afterglow/scripts`.
The directories have this priority, from low to high:

1. The system data directories, in the reverse of their XDG order.
2. `~/.local/share/afterglow/scripts`.
3. `scripts/` beside the active configuration file.

When two scripts have the same ID, the script in the higher-priority directory loads.
In one directory, a package wins over a single file with the same ID.
Of two files in one directory with the same ID, the file whose name sorts last loads.
`afterglow ctl scripts` lists the other script with the status `superseded`.

The VM has no direct file or process API.
Installed scripts are trusted local code.
They can request file reads and process launches through host effects.
The VM sandbox does not prevent those explicit operations.

## Wait for results

`afterglow.await(effect)` applies an effect and waits for its result in the same handler.
It returns the result data. On failure, it returns `nil` and the error.
The third value is the complete result event.

```lua
function script.run_command(command, ev)
    if not ev.pane then return nil end
    local name = afterglow.await(afterglow.prompt(ev.pane, "Branch name"))
    if not name then return nil end -- the dialog was canceled
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
The list must have only the keys 1 to n, with no `nil` entries.
A list with a gap or another key, such as the `n` of `table.pack`, causes an error.

```lua
local results = afterglow.await_all({
    afterglow.exec("branch", {"git", "branch", "--show-current"}, {cwd = ev.pane.cwd}),
    afterglow.exec("commit", {"git", "rev-parse", "--short", "HEAD"}, {cwd = ev.pane.cwd}),
})
local branch = if results[1].ok then results[1].data.stdout else nil
```

`afterglow.sleep(ms)` waits from 0 to 600,000 milliseconds.
A sleep is not an effect for the effect budget, but its result counts toward the result limits.
A loop that sleeps for less than about 8 ms reaches the limit of 128 results each second.

Every handler can wait, except `migrate` and `on_unload`.
Module code that runs at load time cannot wait.
A wait inside a coroutine that the script creates is an error.

Each part of a handler between waits has the time limit of the handler type.
An event handler has 20 ms for each part. A command has 100 ms for each part.
A script can have 16 waiting handlers, 48 effects that they wait for and 16 sleeps.
Another wait returns the error code `busy`.
The `busy` result event has `ok`, `kind`, `request_id`, `data` and `error`, but no `terminal`.
An error after a wait counts as a handler failure.
Awaited results count toward the result limits.
Exception: the result of an `afterglow.output` or `afterglow.output_error` with text to a run with a client is not counted each second (see [command limits](#command-limits)).
Replacement, removal or disable discards waiting handlers. They do not continue.

The [shell-region picker](../data/examples/scripts/semantic-regions.luau) waits for four results in one command.
The [project navigator](../data/examples/packages/project-navigator/init.luau) runs its project searches together with `await_all`.

## Effects and results

Helpers create effects. Return an effect or a list of effects from a handler to apply them.
Use `afterglow.await` when the handler needs the result.
`afterglow.request(id, effect)` requests an `on_result` callback instead.
Request IDs must contain 1 to 64 characters.
Use different IDs for requests that require separate tracking.
IDs that start with `afterglow.await:` are reserved for `afterglow.await`.
Afterglow refuses an effect with such an ID that `afterglow.await` did not give. The effect gets no result.
The `exec` and `read_file` helpers use the task name as the request ID.
So their results also reach `on_result` when you do not wait for them.

A helper that takes a pane uses the event's pane when the pane is `nil`.
Some events have no pane: `on_load`, `on_config_changed`, and a command that runs without a pane.
In these events, such an effect fails with `invalid_effect`.
Give a pane explicitly, for example `ev.terminal.active_pane`.

```lua
function script.run_command(command, ev)
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

Common errors are `invalid_effect`, `no_pane`, `rate_limited`, `operation_failed`, `busy`, `cancelled` and `unavailable`.
The output effects of a command also give `no_run` (see [the handler](#the-handler)).
Direct creation effects return `data.pane`, `pane_id`, `tab_id` and `window_id`.
Other effects return an empty data table unless their reference specifies other fields.

Afterglow queues results for the originating script instance after it applies the operation.
The result records the operation at application time.
Its snapshot can already show that a new pane has closed.
Later effects validate the ID again.

Success confirms operation acceptance or application.
For a generic `action`, it does not confirm that a process or a dialog finished.
`action` cannot run a script command (`script:<id>.<command>`). Such an action fails.
`action` fails while a prompt or a script dialog is open in the window.
A generic `action` runs in the pane's tab, and shows that tab first.
`close_tab`, `rename_tab`, `move_tab_left`, `move_tab_right` and `detach_tab` act on the pane's tab and do not show it, as the tab menu does.
`close_tab` asks first when a program runs in the tab and `Terminal.confirmClose` is on.
`exec`, `select`, `prompt` and `overlay` deliver their results when the process or the dialog finishes.
Use direct creation helpers when you need a returned pane ID.

`copy` fails when the text is longer than 4,194,304 UTF-16 units. Afterglow does not cut it.
`open_url` opens only `http`, `https` and `mailto` URLs. Another scheme fails.
`open_url` also fails when the desktop cannot take the request.
Success does not confirm that a program opened the URL.
These failures return `operation_failed`.

Replacement, removal or disable cancels outstanding results.
An unchanged script keeps its VM and pending results during reload.
Effect lists execute in order. They are not transactions.
A `nil` entry in a returned list is skipped.
An effect with a malformed request ID is refused and not applied. It receives no result.

## Execution limits

| Limit | Value | At the limit |
| --- | --- | --- |
| VM memory | 16 MiB | The call fails |
| Module code at load time | 1,000 ms | The script does not load |
| Event handler, `on_result`, `on_unload` | 20 ms | The call fails |
| Command, package `on_load` and `migrate` | 100 ms | The call fails |
| Handler failures in a row | 5 | The script is disabled |
| `on_timer` and `on_file_changed` calls | 64 each second | Afterglow drops the call |
| Other event handler calls (see the notes) | 64 each second | Afterglow drops the call |
| Effects in one returned list | 64 | No effect applies |
| Values in a returned table, nested tables included | 262,144 | The call fails. A handler that waits for these effects stops |
| Text in a returned table, each use of a string counted again | 64 MiB | The call fails. A handler that waits for these effects stops |
| Applied effects | 20 each second | `rate_limited` |
| Applied `dock` effects | 60 each second | `rate_limited` |
| Structural effects | 3 each 10 seconds | `rate_limited` |
| Queued results | 128 | The script is disabled |
| Result delivery, except output that went out (see the notes) | 128 each second | The script is disabled |
| Waiting handlers | 16 | `busy` |
| Effects that waiting handlers wait for | 48 | `busy` |
| Effects in one `await_all` | 16 | An error in the handler |
| Sleeps | 16, each up to 600,000 ms | `busy` |
| Log message | 2,048 characters | Afterglow cuts the message |

A list with more than 64 effects applies nothing and causes an error.
A disabled script stays disabled until its source or package configuration changes.
Turning scripting off and on again also loads it again.
Refused operations also count toward result limits.
Only effects with a request ID, or that a handler waits for, get results.
The result limits are more than the effect limit and the `dock` limit together.
So the results of the effects that those two limits let through cannot disable a script.
Requested `sleep`, `cancel`, `end_key_mode`, `undock`, `output`, `output_error` and `exit` effects are outside both limits, and their results add to the count.
The result of an `output` or `output_error` with text, to a run with a client, is not counted each second. `Scripts.commandOutputLimit` and `Scripts.commandTimeout` bound it.
An `output` with empty text writes nothing, so its result counts. A run with no client drops its stdout at once, so the result of its output counts too.
A run from the palette, a key, the menu or `afterglow ctl action` has no client.
Thus such a run that waits for each of more than 128 lines in a second disables its script.
Wait for each line only when `ev.source` is `shell`.
Do not wait for each line when `ev.source` is `control`, because `afterglow ctl action` also starts runs with that source.
`on_load`, `on_key`, `on_dock_click`, `on_dock_hover` and `on_dock_layout` calls do not count toward a handler call limit.
`run_command` calls do not count either. The [command limits](#command-limits) bound them.
The `start`, `end` and `cancel` calls of `on_dock_drag` do not count either.
An `on_dock_click` from an accessibility client's press counts, because a client can press without a limit.
`on_dock_wheel` calls count, because a touchpad can send many in a second.
The `move` calls of `on_dock_drag` count, because a fast drag crosses many cells in a second.
The next `move` or the `end` tells the item and the cell that a dropped `move` would have told.
Timers and watchers have their own limit, so events cannot use it up.
A dropped one-shot timer does not fire again.
A repeating timer fires at its next interval.
A dropped `on_file_changed` call is not sent again.
The next change to the file sends a new one.
A dropped `on_dock_wheel` call is not sent again. Its steps are lost.
`sleep`, `cancel`, `end_key_mode`, `undock`, `output`, `output_error` and `exit` do not count toward the effect limit.
`dock` effects have their own limit and do not count toward the effect limit.
Structural effects create, close or move panes, tabs or windows.
They are `split`, `new_tab`, `new_window`, `close`, `move_pane`, `detach_tab`, `open_directory` and `open_project`.
`move_tab` and the `move_tab_left` and `move_tab_right` actions change the order of the tabs. They are not structural.
Generic actions that do the same use the same structural budget.

Afterglow keeps a broken source file from running again.
It loads the file again only after the file changes.

## Terminal state

Each handler receives `ev.terminal`, a version 1 metadata snapshot.
It contains `windows`, `tabs`, `panes`, `active_window`, `active_tab` and `active_pane`.
It does not contain terminal text.
Changing the snapshot does not change the terminal.
Afterglow captures the snapshot before dispatch.
A queued event can describe an earlier command than the snapshot.
`ev.state` in `on_status` remains the program status string.
When a program clears its last status, or the terminal resets, `ev.state` is `idle` and `ev.text` is empty.

| Record | Fields |
| --- | --- |
| Window | `id`, `focused`, `active_tab`, `active_pane`, `project`, `project_directory`, `overview`, `cols`, `rows` |
| Tab | `id`, `window`, `index`, `title`, `active`, `active_pane` |
| Pane | `id`, `window`, `tab_id`, `tab`, `title`, `cwd`, `command`, `cols`, `rows`, `focused`, `context` |

`cwd` is the local directory that the shell reported last, or the working directory of the pane's foreground process.
It is `""` when it is not known.
Afterglow ignores a reported directory that names another host, such as the directory of an `ssh` session.
A reported path without a host is used when that path exists on this machine.

A window's `cols` and `rows` are the cells of the terminal font that fit in the complete window.
A `top` or `bottom` window area that shows is `cols` cells wide.
A pane's `cols` and `rows` are the cells of its text, without the areas of the pane.

A tab is active when its window selects it.
Each unfocused window keeps its selected tab and pane.
Top-level active IDs are zero when no Afterglow window has keyboard focus.
Pane, tab and window IDs exist only during the process lifetime.
`tab_id` remains stable during tab reordering.
`pane.tab` and the `tab` field of the [pane object](CONTROL.md#pane-object) are zero-based positions.

Closed-pane events retain `id`, `tab_id` and `window`.
The closed pane is absent from the live snapshot.
Use these IDs to update surviving panes in the affected tab.

`afterglow.panes(state, scope, anchor)` filters an existing snapshot.
Scope is `all`, `pane`, `tab` or `window`. The default is `all`.
Another scope causes an error in the handler.
The anchor can be a pane record or ID.
Without an anchor, the helper uses the globally active objects.
A missing anchor ID gives no matches, in every scope.

```lua
function script.run_command(command, ev)
    local panes = afterglow.panes(ev.terminal, "tab", ev.pane)
    return afterglow.log("This tab has " .. #panes .. " panes")
end
```

## Events and lifecycle

Pane events include `on_pane_created`, `on_pane_closed`, `on_pane_moved`, `on_focus`, `on_bell` and `on_title`.
Other events are `on_status`, `on_command_started`, `on_command_finished`, `on_user_var`, `on_context` and `on_tab_switch`.
These events contain the applicable pane and terminal snapshot.
`on_command_started` and `on_command_finished` report the commands that a pane's shell runs, through shell integration. They are not the script [commands](#commands).
The events of docked areas are `on_dock_click`, `on_dock_hover`, `on_dock_wheel`, `on_dock_drag` and `on_dock_layout` (see [docked areas](#docked-areas)).

A split's `on_pane_created` also contains `split_from`, the pane it split from, and `edge`.
`edge` is the side the two panes share: 1 left, 2 top, 3 right or 4 bottom.
`on_tab_switch` contains `from_tab`, `to_tab` and `direction`, 1 toward a tab on the right and -1 toward one on the left.
Its pane is the new tab's focused pane.
A tab that moves to another place in its window sends `on_pane_moved` for each of its panes.
The other tabs also change their `index`, but they send no event.

`on_user_var` runs when a variable changes. Setting the same value again sends no event.
A program can set a title or a variable faster than Afterglow delivers the events.
`on_title` and `on_user_var` then get the newest value, and the values in between are skipped.
Do not use user variables to send a sequence of messages.

`run_command(command, ev)` handles the script's [commands](#commands).
`on_load`, `on_unload` and `on_config_changed` handle lifecycle changes.
Handlers and `metadata` must be fields of the table that the script returns.
Afterglow does not read them through a metatable.
`on_config_changed` runs after the Afterglow configuration changes. It has no pane.
A change to a package's own `config.json` replaces the package instance instead.
A single-file script's `on_load` has the 20 ms event limit. A package's `on_load` has 100 ms.

`on_timer` and `on_file_changed` contain the task name.
They receive the pane of the handler that started the task, when that handler had one.
Effects without a pane then go to that pane.

A broken replacement keeps the last working instance.
Successful replacement, removal, disable or five handler failures in a row cancel tasks and remove UI contributions.
`on_result` and `on_unload` have the 20 ms event limit. The returned effects of `on_unload` are ignored.
A handler that continues after `afterglow.await` keeps the limit of its handler type.

## Tasks

Task names contain 1 to 64 characters and belong to one script instance.
Limits are 16 active tasks for each script and 128 in total.
Reusing an active name returns `busy`.
`afterglow.cancel(name)` cancels the owned timer, watcher or job.

| Effect | Behavior |
| --- | --- |
| `timer(name, delay_ms, repeat_ms?)` | Calls `on_timer` with `ev.name` |
| `exec(name, argv, options?)` | Starts an asynchronous process and returns a result with the task name |
| `read_file(name, path, options?)` | Reads a regular file asynchronously and returns `text` and `truncated` |
| `watch_file(name, path)` | Monitors an existing regular file and calls `on_file_changed` |

A script that has 16 active tasks, or a total of 128 active tasks, gets `rate_limited`.

| Timer field | Range |
| --- | --- |
| `delay_ms` | 1 to 3,600,000 ms. `afterglow.timer` requires it |
| `repeat_ms` | 0 for one callback, or 250 to 3,600,000 ms |

The next interval starts when the handler returns or first waits.
So an `on_timer` that waits can still wait when the next tick comes.
Waiting handlers then collect until waits return `busy`.
For slow work, use a single timer and start it again at the end of the handler.
Watcher events contain `name`, `path` and `exists`.
Watchers handle atomic file replacement and combine changes within 250 ms.

`read_file` and `watch_file` need an absolute path of at most 4,096 characters.
Process options include `cwd`, `env`, `timeout_ms` and `max_bytes`.
`cwd` must be an absolute path.
Without `cwd`, the process runs in the working directory of Afterglow.
A command name without `/` is found in the `PATH` of the child environment, which `env` can change.
Relative `PATH` entries start from the process working directory.
A command that is not found gives `spawn_failed`.
`argv` has 1 to 64 arguments of at most 4,096 characters each.
`argv` can also be a string. The string is then the program alone, with no arguments. Afterglow does not split it at spaces and does not use a shell.
Each entry of the `argv` list must be a string. Another value, such as a number, refuses the effect with `invalid_effect`.
A task name that is not a string of 1 to 64 characters also refuses the effect with `invalid_effect`.
File reads accept the timeout and byte-limit options.
Timeout defaults to 30,000 ms and cannot exceed 600,000 ms.
The output limit defaults to 65,536 bytes and cannot exceed 1 MiB.
At most eight file reads run together. Another read gets `busy` at once.
Special files are not permitted.

Normal process results contain `stdout`, `stderr`, `exit_code` and `truncated`.
Errors include `spawn_failed`, `process_failed`, `timeout`, `output_limit`, `cancelled` and `io_error`.
Exceeding the combined stdout/stderr limit stops the job.
On `output_limit`, the third value of `afterglow.await` has `data` with `stdout`, `stderr` and `truncated`.
On `process_failed`, that `data` also has `exit_code`.
`timeout` and `cancelled` return no output.
When the process ends, Afterglow stops the other processes of its process group.
Timeout, cancellation and unload also stop the process group.
Afterglow discards callbacks for retired instances.

## Native UI

Wait for the result of `select(pane, title, items)` or `prompt(pane, title, initial_text?)` with `afterglow.await`.
You can also wrap the effect with `afterglow.request`.
Selectors accept 1 to 500 unique `{id, title}` items and provide fuzzy filtering.
An item without a `title` shows its `id`.
Selection results contain `id` and `title`. Prompt results contain `text`.
Escape, pane closure or window closure cancels a dialog.
A second dialog in the same window returns `busy`.
A dialog also returns `busy` while an [overlay](#overlays) has the focus, or when its pane has an overlay.
A pane that closed before the dialog opened returns `no_pane`.
Dialogs use the desktop Qt Quick Controls theme.

`status(pane, text?, tab_title?)` replaces the status label and tab suffix.
Empty strings clear these contributions.

| Text | Limit |
| --- | --- |
| Selector item `id` | 1 to 64 characters |
| Selector item `title` | 512 characters |
| Dialog title | 512 characters |
| Prompt text | 4,096 characters |
| `status` text | 512 characters |
| `status` tab suffix | 128 characters |
| Key mode label | 160 characters |

A text over its limit, or a repeated `id`, refuses the complete effect with `invalid_effect`.

`override(pane, values)` replaces the owner's pane appearance settings.
Supported fields are `cursorBlink`, `backgroundOpacity` and `unfocusedDim`.
The last two fields accept values from zero to one.
The latest script value for each field takes priority over configuration.
Removal exposes the next owner's value or the configured value.
An empty table clears the owner's values.
Unload and pane closure remove all contributions.

## Overlays

An overlay runs a program over a pane and returns the result to the script.
The program gets a terminal of its own, in the place of the pane, with the input focus.
The pane under it keeps running, and its program keeps its output.
Use an overlay for an interactive picker, such as fzf.

```lua
local items = {"alpha", "beta", "gamma"}
local r, err = afterglow.await(afterglow.overlay(ev.pane, {"fzf", "--multi"}, {
    stdin = table.concat(items, "\n"), capture = "stdout", cwd = ev.pane.cwd, title = "Pick",
}))
if r and r.exit_code == 0 then
    return afterglow.send_text(ev.pane, r.stdout)
end
```

`overlay(pane, command, options?)` takes the program and its arguments as `exec` does: a list of 1 to 64 strings of at most 4,096 characters each, or a string that names the program alone.

| Option | Behavior |
| --- | --- |
| `stdin` | Text for the program's standard input, at most 16 MiB. Without it, the program reads the terminal. |
| `capture` | Where `stdout` of the result comes from. `"stdout"` (the default) reads the program's standard output. `"file"` sets `AFTERGLOW_RESULT` to the path of a new file that only you can read, and reads that file. `"none"` returns an empty `stdout`. |
| `cwd` | The program's directory. When it is not a local directory, for example a path that a remote shell reported, the program starts in `$HOME`. |
| `env` | Variables for the program, as for `exec`. `false` removes a variable. |
| `title` | Shows as the pane's title while the overlay runs, at most 512 characters. |

The terminal stays the program's controlling terminal and its standard error, so the program can draw on `/dev/tty`.
With `stdin` and `capture = "stdout"`, a program like fzf reads its list from stdin, draws on `/dev/tty` and writes its choice to stdout.
Standard output that is not captured goes to the terminal.
The program gets the pane's `AFTERGLOW_PANE_ID`, so `afterglow ctl` in it acts on the pane.

| Result field | Content |
| --- | --- |
| `exit_code` | The program's exit status. A program that a signal ended gives 128 plus the signal number. |
| `stdout` | The captured output, at most 1 MiB of UTF-8 text |
| `truncated` | `true` when the output was over 1 MiB and was cut |

A non-zero exit status is a result, not an error: fzf exits with 130 at Escape and with 1 when nothing matches.

| Error code | Cause |
| --- | --- |
| `no_pane` | The pane has closed. |
| `busy` | The pane has an overlay already, a script dialog is open in the window, or another script's key mode holds the keyboard. |
| `spawn_failed` | The program cannot start. |
| `cancelled` | The overlay closed before the program exited. |

The overlay closes when its program exits.
It also closes with `cancelled`, and its program gets `SIGHUP`, in these conditions:

- The pane under it closes.
- The script stops: a change, a removal or a disable.
- The command run that waits for it ends (see [Cancel](#cancel)).

The overlay fades in and out over the pane, through the `Shaders.overlayTransition` pack.
The default pack, Overlay Fade, blends the pane and the overlay. `Animation.overlayCurve` and `Animation.overlayDuration` set the fade.
With an empty `Shaders.overlayTransition`, the overlay shows and goes at once.
When the overlay closes, the focus goes back to the pane. The pane's cursor and view stay where they were.

| Situation | Behavior |
| --- | --- |
| The pane overview, other tabs | The overlay shows where its pane is. |
| A moved pane, a detached tab | The overlay goes with its pane. |
| Zoom, a resize | They apply to both. The program gets the pane's new size. |
| Saved projects | Overlays are not saved. |
| Copy mode, search, hints, paste, the mouse | They act on the overlay while its pane has the focus. |
| Assistive technology | It reads the overlay's text in place of the pane's. |
| The pane's shaders | The pane's layer and pane shaders draw the overlay. During a fade, they draw the pane and the overlay both. |

An overlay is not a pane of its own. `afterglow ctl ls` and the terminal snapshot do not show it.
`afterglow ctl overlay` opens an overlay from a shell (see [CONTROL.md](CONTROL.md#overlays)).
The [fzf picker package](../data/examples/packages/fzf-picker/README.md) uses fzf when it is installed, and the select dialog when it is not.

## Commands

A command is an operation that a script adds.
You can run it from a shell, from the command palette, from the context menu or with a key.
A single-file script declares its commands in `metadata.commands`.
A package declares them in `package.json` (see [packages](PACKAGES.md#commands)).

```lua
local script = {}
script.metadata = {
    commands = {
        open = { summary = "Open a project directory", usage = "DIR [LAYOUT]", palette = "Open project…" },
        list = { summary = "Print the saved projects" },
        import = { summary = "Add projects from a list on stdin", stdin = true },
    },
}
```

| Field | Rule |
| --- | --- |
| Command name | 1 to 64 characters: letters, digits, `_` and `-`. It must not start with `-`, because `aglow run` reads that as an option |
| `summary` | Required, not blank. At most 512 UTF-16 units |
| `usage` | Optional. The arguments, for help. At most 512 UTF-16 units |
| `palette` | Optional palette title, not blank, at most 512 UTF-16 units. Commands that have it appear in the palette and the context menu |
| `stdin` | Optional boolean, default `false`. Only a command with `stdin = true` gets the client's stdin |
| Commands of a script | At most 64, with those of `set_commands` |

`summary`, `usage` and `palette` must not contain control characters, such as a line break or a tab.
They also must not contain the line separator (U+2028), the paragraph separator (U+2029) or a bidirectional control.
The bidirectional controls are U+061C, U+200E, U+200F, U+202A to U+202E and U+2066 to U+2069.
The command palette shows `<id>: <palette>`, for example `marker: Mark this pane`.
The Scripts submenu of the context menu shows the `palette` title alone.

A manifest that breaks a rule stops the script from loading. `afterglow ctl scripts` shows the error.
A script with commands must have a `run_command` function.

### Run a command

| Source | How |
| --- | --- |
| `shell` | `aglow run <id> <command> [ARGS…]` (see [control](CONTROL.md#aglow-run)) |
| `palette` | The command palette, for a command with `palette` |
| `menu` | The Scripts submenu of the context menu, for a command with `palette` |
| `key` | `script:<id>.<command>` in `Keybinds.bindings`, in a key table or in a key sequence (see [input](INPUT.md)) |
| `control` | `afterglow ctl action script:<id>.<command>`, or a `SCRIPT.COMMAND` call of the `commands` target |

The palette, a key, the menu and `afterglow ctl action` pass no arguments.
The handler of a command with `palette` must work with `args` empty.
`afterglow ctl action` starts the command and returns at once. `aglow run` waits for the end of the run.

### The handler

`run_command(command, ev)` handles every command of the script.

| `ev` field | Content |
| --- | --- |
| `args` | List of strings, at most 256, each at most 4,096 UTF-16 units |
| `cwd` | The client's directory. Else the pane's directory. Else an empty string, for a run with neither |
| `pane` | For a shell run: `--pane N`, else the pane that runs the shell, else the focused pane. For `control`, the pane that `afterglow ctl` names, else the focused pane. For other sources, the pane of the palette, key or menu. Only `{ id }` when the pane closed before the run started. `nil` when there is no pane |
| `stdin` | All of the client's stdin as one string, for a command with `stdin = true` that runs from the shell. `nil` for another source. `nil` also when `aglow run` reads no stdin: with `-n`, or when stdin is a terminal. A run with more stdin than `Scripts.commandStdinLimit` does not start. `aglow run` refuses stdin that is not UTF-8 text and exits with status 2 |
| `source` | `"shell"`, `"palette"`, `"key"`, `"menu"` or `"control"` |
| `run` | The run ID, for the output effects |
| `terminal` | The terminal snapshot |

| Effect | Behavior |
| --- | --- |
| `afterglow.output(run, text)` | Writes text to the client's stdout. When you wait for it, it returns after the text has gone to the client |
| `afterglow.output_error(run, text)` | Writes text to the client's stderr |
| `afterglow.exit(run, code)` | Sets the exit status, 0 to 255. A run that sets no status exits with 0 |

`run` is `ev.run`, or the event itself.
A script can write only to its own runs. Output to a run that has ended, or that is not the script's, fails with `no_run`.
Exception: the handler of the run itself can still write after the run ends. That output goes nowhere (see [cancel](#cancel)).
When you wait for output to a run that ends before the client gets the text, the result is `cancelled`.

The [project navigator's](../data/examples/packages/project-navigator/init.luau) `import` command reads its stdin.

```lua
function script.run_command(command, ev)
    if command == "list" then
        local projects, err = afterglow.await(afterglow.list_projects())
        if not projects then
            return { afterglow.output_error(ev, err.message .. "\n"), afterglow.exit(ev, 1) }
        end
        return afterglow.output(ev, table.concat(projects.names, "\n") .. "\n")
    end
end
```

### The end of a run

- The run ends when the handler returns, also after it waited with `afterglow.await`.
- The client then gets the exit status.
- When the handler fails, the run ends with exit status 1. The error goes to stderr.
- A failure counts as a handler failure (see [execution limits](#execution-limits)).
- A run can end inside a returned list, at its output limit or when its client left. The effects after that point do not apply. `dropped_effects` in the diagnostics counts them.

A run with no client (from the palette, a key, the menu or `afterglow ctl action`) drops its stdout.
When it writes to stderr or ends with a status other than 0, a notification shows on its pane.
A run with no pane shows a desktop notification that belongs to no pane.
The notification has the palette title of the command, else `<id> <command>`.
It shows the last 2,048 characters of stderr, with their line breaks.
These notifications count against `Notifications.rateLimit` of the pane, or against one limit for all notices without a pane.

### Command limits

| Limit | Value | Past the limit |
| --- | --- | --- |
| Each part of the handler between waits | 100 ms. You cannot change it | The run ends with exit status 1 |
| The total time of a run, `Scripts.commandTimeout` | 1,000 to 3,600,000 ms. Default 600,000 ms | The run ends with exit status 124 |
| Runs of one script at once, `Scripts.commandRunLimit` | 1 to 16. Default 16 | A new run fails with exit status 1 and a message |
| Stdin, `Scripts.commandStdinLimit` | 0 to 4,096 KiB. Default 1,024 KiB. 0 refuses any input that is not empty | The run does not start. `aglow run` exits with status 2 |
| UTF-8 bytes of stdout and stderr together, `Scripts.commandOutputLimit` | 64 to 262,144 KiB. Default 16,384 KiB | The run ends with exit status 1 and a message |

One command can have several runs at once.
The runs of one script share its 16 waiting handlers, 48 awaited effects, 16 sleeps and 16 tasks (see [execution limits](#execution-limits)).
The output effects and `exit` are outside the effect budget. `Scripts.commandOutputLimit` bounds them.
A run with no client drops its stdout, so its stdout does not count toward `Scripts.commandOutputLimit`. Its stderr counts.
A change to a limit applies to the runs that start after it.

`Scripts.commandTimeout` counts all the time of a run.
This includes a wait for a task, for a dialog, and for a client that reads its output slowly.
It does not include the time that `aglow run` sends stdin before the run starts.
A client that reads its output slowly holds the run back.
A wait for `afterglow.output` then takes longer. The output that waits counts toward `Scripts.commandOutputLimit`.

### Cancel

A run also ends when its client leaves, at a cancel, at the time limit and at the output limit.
When its handler waits at that moment, these steps occur:

1. The dialogs, selectors and [overlays](#overlays) that the handler waits for close. The program of an overlay gets `SIGHUP`.
2. The processes (`exec`) and file reads (`read_file`) that it waits for stop. Its sleeps stop. Their task names and sleep slots are free again.
3. The handler resumes once, as from `afterglow.await`, with `nil` and the error code `cancelled`. The error message tells why the run ended.
4. `afterglow.await_all` gives the results that came before the end, and `cancelled` for the others. An effect that applied, but that had no result yet, also gives `cancelled`.

The handler resumes after the current handler call returns, not during it.
The `cancelled` result events have `terminal`, and `pane` when the run has a pane.
A result that was not delivered before the run ended does not resume the handler. The handler resumes once, with `cancelled`.

The handler can then clean up, for example clear a busy flag after a `pcall`.
This part has the 100 ms limit of each part of a command.
The cleanup can wait again. Each later part also has the 100 ms limit.
`Scripts.commandTimeout` does not stop the cleanup, because the run has ended.
Its waits count toward the 16 waiting handlers of the script until the handler returns.
Its `afterglow.output`, `afterglow.output_error` and `afterglow.exit` go nowhere, because the client has its exit status already. They are not errors.
Its other effects apply as usual.
An error in this part is not a handler failure. It ends the script's key mode, as every handler error does.

A run also ends when its script stops: a change, a removal, `Scripts.enabled=false` or a disable.
The run then ends with exit status 1, and stderr tells why.
A script is disabled after 5 handler failures in a row, or after too many results (see [execution limits](#execution-limits)).
The handlers of a stopped script do not resume.

### Commands at runtime

`afterglow.set_commands(commands)` sets the commands that the script adds while it runs.
`commands` has the shape of `metadata.commands`: each command name maps to the fields of the table above.
The table replaces the commands of the last `set_commands`. The manifest's commands stay.
An empty table removes the commands of the last `set_commands`.
A name that the manifest has, or a field that breaks a rule, refuses the complete effect.

```lua
return afterglow.set_commands({ deploy = { summary = "Deploy the branch", palette = "Deploy" } })
```

The [projects example](../data/examples/scripts/projects.luau) adds a command for each saved project.

## Docked areas

`dock(pane, spec)` puts a docked area (a dock) of styled text at an edge of the pane's window, or of the pane.
The panes get the space that is left, in whole cells.
The shell in each pane gets the new size.
The renderer draws the area like a pane, with the terminal font, the palette and the box-drawing and Powerline characters.
No shader changes a window area.
Each window has its own areas. The same `id` in another window is a different area.
`dock` acts on the window that holds `pane` when Afterglow applies the effect.
With `scope = "pane"`, the area goes at an edge of the pane itself. See [pane areas](#pane-areas).

```lua
return {
    on_focus = function(ev)
        return afterglow.dock(ev.pane, {id="bar", edge="top", tab_bar=true, fill=8, lines={{
            {text=" main ", fg=0, bg=4, bold=true},
            {text="\u{e0b0}", fg=4},
            {spacer=true},
            {text=" 12:30 ", fg=0, bg=2, item="clock"},
        }}})
    end,
    on_dock_click = function(ev)
        return afterglow.log(ev.item .. " " .. ev.button)
    end,
}
```

| Field | Value |
| --- | --- |
| `id` | Required. 1 to 32 ASCII letters, digits, `-` or `_`. A second `dock` with the same `id` replaces the area. |
| `edge` | Required. `top`, `bottom`, `left` or `right`. |
| `size` | Rows for `top` and `bottom`, 1 to 8. Columns for `left` and `right`, 1 to 80. The default is 1. |
| `tab_bar` | `true` draws a `top` area in place of the tab bar. The default is `false`. Only a `top` area with the `window` scope can have `true`. |
| `scope` | `window` puts the area at an edge of the pane's window. `pane` puts it at an edge of the pane. The default is `window`. |
| `fill` | A palette index from 0 to 255, or `"#rrggbb"`. It is the background of the complete area. Without it, the area uses the theme's background. Below a `Window.backgroundOpacity` of 1, the fill has the same opacity as the pane background. |
| `lines` | A list of lines from the top. A line is a list of runs from the left. Without `lines`, the area is empty. |

A `top` or `bottom` area has at most `size` lines.
Lines past the bottom of a `left` or `right` area do not show.

| Run field | Value |
| --- | --- |
| `text` | The text. A control character shows as a space. Text past the edge is cut. A wide character that does not fit shows as an empty cell in the colors of its run. |
| `fg`, `bg` | A palette index from 0 to 255, or `"#rrggbb"`. Indices 0 to 15 follow the theme. |
| `bold`, `italic`, `underline` | `true` or `false` |
| `item` | 1 to 64 characters, with no control characters. A click on the cells of the run sends `on_dock_click`. `on_dock_hover`, `on_dock_wheel` and `on_dock_drag` name the item under the pointer. |
| `spacer` | `true` makes an empty run that takes the cells the other runs of its line leave. Several spacers share them. |

Without `fg`, a run uses the theme's foreground color. Without `bg`, it uses the area's `fill`.
A spacer cannot have `text`.
A run that takes no cells, such as a run with empty text, cannot be clicked.
A field that is not in these tables refuses the effect with `invalid_effect`.
A value of the wrong type or outside its range also refuses the effect with `invalid_effect`.

`on_dock_click` goes only to the script that owns the area.
Afterglow sends it when a mouse button goes down on a run with an `item`, or when an accessibility client presses the item.

| Event field | Value |
| --- | --- |
| `dock` | The `id` of the area |
| `item` | The `item` of the run |
| `button` | `left`, `middle` or `right` |
| `window` | The window ID |
| `scope` | `window` or `pane`, as in the `dock` effect |
| `pane` | For a `window` area, the focused pane of the window, if the window has one. For a `pane` area, the pane of the area. |

Other mouse buttons send no event.
A click on a run without an item, or on empty cells, does nothing.
Accessibility clients see each item as a button. Its press sends the event of a left click.
See [desktop integration](DESKTOP.md#docked-areas).
Clicks and the mouse wheel on an area never reach the panes.
A click on a pane area does not focus its pane. The script can return `focus(ev.pane)` to do that.
While the pane overview is open, a window area takes clicks too.
A click on an item then sends `on_dock_click`, and the overview stays open.
A pane area gets no clicks while the overview is open (see [pane areas](#pane-areas)).
A prompt that is open over the overview takes the click first.
An area always reads from left to right. `Terminal.bidi` does not apply to areas.

`on_dock_hover` goes to the owner when the item under the pointer changes.
It contains `dock`, `scope`, `window` and `pane` as above, and `item` when the pointer is on an item.
Without `item`, the pointer has left the items of that area.
New text that puts another item under a pointer that does not move also sends the event.
When the script removes the area under the pointer with `undock`, the script gets the event without `item` first.
The pointer is on no item while a prompt, a menu or the command palette is open, and while it is over the search bar.
The pointer is also on no item during a drag that started in a pane or on a divider.
The same is true for a drag that started on the tab bar, on the scrollbar or in the overview.
Keep the cells of an item the same when it shows its hover state. Otherwise the item under the pointer can change again.

`on_dock_wheel` goes to the owner when the mouse wheel turns over an area.
It contains `dock`, `scope`, `window` and `pane` as above.

| Event field | Value |
| --- | --- |
| `direction` | `up`, `down`, `left` or `right` |
| `steps` | The whole wheel notches, 1 or more. Part of a notch waits for the next event over the same item. |
| `item` | The item under the pointer, when there is one |

`on_dock_drag` goes to the owner when the left mouse button drags an item of an area.
The press sends `on_dock_click` first.
The drag starts when the pointer moves the system drag distance from the press.
It contains `dock`, `scope`, `window` and `pane` as above.

| Event field | Value |
| --- | --- |
| `phase` | `start` when the drag starts, `move` when a pointer move changes `over`, `col` or `row`, `end` when the button goes up, `cancel` when the drag stops in another way |
| `item` | The item that the press was on |
| `over` | The item of this area under the pointer, when there is one |
| `col`, `row` | The cell under the pointer, from 0 at the top left of the area. Outside the area, they are less than 0, or equal to its size or more. |

A press and release with no drag sends no `on_dock_drag`.
The drag continues outside the area and outside the window, until the button goes up.
The drag gets `cancel` when the script removes the area with `undock`.
It also gets `cancel` when the area goes with its pane or window.
A drag on a pane area that stops showing gets `cancel` at the next pointer move or release.
A release that Afterglow does not get, such as a release in another window, sends `cancel` at the next pointer move.
New text that puts another item under a pointer that does not move sends no `move`.
The next pointer move or the `end` has the new `over`.
`on_dock_hover` continues during the drag.

`move_tab(pane, index)` moves the pane's tab to `index` in its window, and does not show it.
`index` is the `index` of a tab in the snapshot, from 0.
An `index` that is not a tab of the window fails with `operation_failed`.
An `index` that is not a whole number of 0 or more is refused with `invalid_effect`.
The index of the tab's own place succeeds and does nothing.
The tab line package uses `on_dock_drag` and `move_tab` to drag tabs.

`on_dock_layout` goes to the owner after Afterglow first places an area, also when the area does not fit.
It also goes when the area starts or stops showing, and when its size changes.
An `undock` sends no `on_dock_layout`.
Use the event to lay out the text for the cells that the area has.
It contains `dock`, `scope`, `window` and `pane` as above.

| Event field | Value |
| --- | --- |
| `shown` | `true` when the area shows |
| `cols`, `rows` | The cells of the area. Both are 0 when it does not show. |

New text in the same place sends no `on_dock_layout`.
A change of the window size, the font or another area can send it.
While the window changes size or a divider moves, it can come once in each frame.
A script that draws again for each one can get `rate_limited` at the `dock` limit.
Send the last text again later, as the tab line package does.

`tab_menu(pane)` opens the tab menu of the pane's tab, as a right click on the tab bar does.
The menu opens at the last click on an item of an area of that window, or at an accessibility client's press.
Each menu uses that position once. A click on another part of the window also clears it.
Without a position, the menu opens at the pointer. When the pointer is outside the window, the menu opens at the top left corner.
While a prompt, a script dialog or the command palette is open in the window, the result is `busy`.
A menu that is open closes, and the tab menu opens in its place.

Areas on one edge stack inward from the window's edge, in the order that they were placed.
`tab_bar` areas are the outermost areas on the top edge, where the tab bar is.
A `top` area without `tab_bar` shows below the tab bar.
`top` and `bottom` areas take the full width. `left` and `right` areas fit between them.
A `tab_bar` area shows for every `Window.tabBar` value, also in a window with one tab.
An area that would leave the panes smaller than 4 columns or 2 rows does not show. Its `dock` effect still succeeds.
New text for an area keeps its place. A change of `edge`, `size` or `tab_bar` puts it last on its edge.

`undock(pane, id)` removes the area from the pane's window.
When that window has no area with this `id`, or the pane has closed, `undock` looks elsewhere.
It removes an area whose most recent `dock` named that pane. When several did, the newest goes.
An `id` that the script has no area for is not an error.
If the pane has closed and no such area names it, the result is `no_pane`.
The tab bar shows again when the last `tab_bar` area is removed.
A `tab_bar` area that does not show leaves the tab bar in place.
Successful replacement, removal and disable of the script remove all of its areas.
Five handler failures in a row disable the script, so they also remove its areas.
When a window closes, its areas close with it.

| Limit | Value | At the limit |
| --- | --- | --- |
| `dock` effects of one script | 60 each second | `rate_limited` |
| Areas of one script in a window | 8 | `rate_limited` |
| Areas in a window | 32 | `rate_limited` |
| Lines of a `left` or `right` area | 256 | `invalid_effect` |
| Runs in a line | 128 | `invalid_effect` |
| Runs in an area | 1,024 | `invalid_effect` |
| UTF-8 text and items of an area | 16,384 bytes | `invalid_effect` |

A new area past a count limit gets `rate_limited`. Remove an area to make room.
New text for an existing area is not refused at the count limits.

The [tab line package](../data/examples/packages/tab-line/init.luau) draws the tab bar of each window as a docked area.
A click switches tabs, a middle click closes a tab and a right click opens its tab menu.
The package also has a `+` button, moves through the tabs with the wheel and shows the tab under the pointer.
Drag a tab to move it.

### Pane areas

A `dock` with `scope = "pane"` puts the area at an edge of the pane.
The pane's text gets the space that is left, in whole cells, and its shell gets the new size.
The other panes keep their size.
Each pane has its own areas. The same `id` at another pane, or in the window scope, is a different area.

```lua
local function title(pane, text)
    return afterglow.dock(pane, {id="title", edge="top", scope="pane", fill=8, lines={{
        {text=" " .. text .. " ", fg=0, bg=4, item="title"},
    }}})
end

return {
    on_pane_created = function(ev) return title(ev.pane, "ready") end,
    on_command_started = function(ev) return title(ev.pane, ev.command) end,
    on_command_finished = function(ev) return title(ev.pane, "ready") end,
}
```

New text keeps the area in its place, so the pane and its program keep their size.
Do not add and remove an area for each command: each change gives the program a new size.

A pane area is part of the pane:
- It moves and changes size with the pane in split animations.
- It shows on the pane's card in the pane overview, in a tab transition and in the frame of a pane that closes.
- The pane shaders, the text and background layer shaders and the window shaders draw over it, as they draw over the pane's text.
- It goes with the pane to another tab or window, and it closes with the pane.

The events of a pane area have `scope = "pane"`, and their `pane` is the pane of the area.
While the pane overview is open, the cards take the pointer: a pane area gets no click, hover, wheel or drag.
A pane that a zoom hides shows none of its areas.
The layout rules of window areas apply inside the pane.
An area that would leave the pane smaller than 4 columns or 2 rows does not show.
A pane in a tab that does not show lays out its areas in the last layout of its tab.
Its shell gets the new size at once, unless a zoom hides the pane.
Window areas are the outermost: they take their space from the window first, and the panes share the rest.

`undock(pane, id, "pane")` removes the area from the pane.
When the pane has closed, its areas are gone, and the result is `no_pane`.

| Limit | Value | At the limit |
| --- | --- | --- |
| Areas of one script at a pane | 4 | `rate_limited` |
| Areas at a pane | 8 | `rate_limited` |

The other limits of areas apply to pane areas too. Pane areas do not count toward the window limits.

## Key modes

`key_mode(pane, label?)` sends the keys of the pane's window to the script's `on_key` handler.
The keys do not reach the shell. The window shows the label.
A script needs an `on_key` handler to start a key mode.
One script at a time holds a window's keyboard. Another script's request returns `busy`.

`on_key` receives `key`, `text`, `ctrl`, `shift`, `alt`, `meta` and `repeat`.
`key` is the typed character, such as `a` or `A`. Other keys have a name:

| Keys | `key` |
| --- | --- |
| Editing | `Enter`, `Escape`, `Tab`, `Backspace`, `Delete`, `Insert`, `Space` |
| Movement | `Home`, `End`, `PageUp`, `PageDown`, `Left`, `Right`, `Up`, `Down` |
| Function keys | `F1` to `F35` |
| Other keys | The lowercase Qt key name, such as `menu` |

A modifier key alone does not reach `on_key`. It comes with the key that it changes, in `ctrl`, `shift`, `alt` and `meta`.
With Ctrl, `key` is the letter, and `text` is empty.
Ignore the keys that the script does not use.
`end_key_mode()` gives the keyboard back. It works also when the effect budget is spent.
A handler error, disable, replacement or removal of the script also ends its key mode.
The `end_script_key_mode` keybinding always ends a key mode. The default is Ctrl+Shift+Escape.
The script does not receive that key.
Key releases are not reported.

## Pane overview

`overview(pane, operation)` drives the pane overview of the pane's window.
`open` opens it from the pane. `close` closes it and keeps the focus. `choose` focuses the highlighted pane.
`open` fails with `operation_failed` while another script holds a key mode.
The default operation is `open`.
When the chosen pane is in another tab, that tab shows as the overview closes.
`overview_move(pane, step)` moves the highlight by `step` cards. `step` is a whole number from -32 to 32.
The default `step` is 0.
`overview_move(pane, 0, target)` highlights the pane `target`. `target` is a pane record or ID.
Results contain the overview: `open`, `highlighted` and `cards`.
The snapshot's window records contain the same `overview` table.
`cards` lists the pane IDs of every tab in tab order while the overview is open, otherwise the panes of the current tab.

Combine these effects with a key mode to replace the overview's keys.
The [scripted overview package](../data/examples/packages/scripted-overview/init.luau) moves with a, d, w and s, and ships its own overview shader.
A key mode that starts while the overview is open ends when the overview closes.
This also applies when a click or the keyboard closes the overview, so the next key goes to the shell.
A key mode that starts before the overview opens continues.

## Pane pulses

`pulse(pane, effect, options?)` plays an effect over the pane in `Shaders.paneEffects`.
The bundled Pane Effects pack draws these effects:

| Effect | Number | Look |
| --- | --- | --- |
| `portal` | 1 | A ring that sweeps out |
| `flash` | 2 | A light that fades |
| `shake` | 3 | The pane shakes |

Numbers from 4 to 255 select effects that other packs define. Give an effect as its name, or as a number or a numeric string.
A `pulse` without an effect causes an error in the handler.
`options.duration_ms` sets the time, from 50 to 10,000 ms. The default is 600 ms.
`options.color` is a palette index from 0 to 255. The default is 4.
A new pulse on the same pane replaces the old one.
The [pulse example](../data/examples/scripts/pane-pulses.luau) flashes failed commands and shakes on the bell.

## Launch options

`split`, `new_tab` and `new_window` accept a final `LaunchOptions` table.
`cwd` sets the working directory.
`cwd` in the options takes priority over the `cwd` argument of `new_tab` and `new_window`.
A `cwd` of `~` or one that starts with `~/` starts in your home directory.
Another `cwd` must be an existing absolute directory.
Without `cwd`, `split(pane)` and `new_tab(pane)` start in the directory of `pane`, as the `Window.workingDirectory` setting permits.
The command is a program name, or a list of the program and its arguments.
Each entry must be a string. Another value refuses the effect with `invalid_effect`.
Without a command, or with an empty string, the pane starts the default shell.
The tab of `new_tab(pane)` opens after the tab of `pane`.
`env` changes inherited environment variables.
`size` sets the new split fraction, greater than zero and less than one.
Split directions are `left`, `right`, `up` and `down`. The default direction is `right`.
Failed launches return errors and remove the incomplete ordinary pane.

`move_pane(pane, target?, placement?)` moves a pane. The default placement is `right`.
`left`, `right`, `up` and `down` place the pane beside `target`, which is required.
`new-tab` and `new-window` take no target.
Another combination is refused with `invalid_effect`.

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
`max_bytes` limits UTF-8 output to 1 to 1,048,576 bytes (1 MiB). The default is 65,536 bytes.
Another value is refused with `invalid_effect`.
A missing selection or unavailable region returns `available=false`.

`semantic_regions` returns up to 1,024 retained shell regions in chronological order.
Afterglow removes a region when a line deletion, a scroll in a scroll region or a resize destroys its start row.
It also removes a completed region when its end row is destroyed.
A start row that leaves the history does not remove the region (see `start_trimmed`).
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
While it is `false`, `afterglow.action` also fails for `copy`, `paste`, `select_all` and `show_last_output`.
It controls terminal queries, not operating-system file or process access.
A command run can have a client: `aglow run`, or a `SCRIPT.COMMAND` call of the `commands` target.
In the handler of such a run, these queries also need `Control.allowReadText`.
The output of the command can take the text to a process outside the window (see [control](CONTROL.md)).
Other handlers do not need `Control.allowReadText`.

The [shell-region picker](../data/examples/scripts/semantic-regions.luau) copies a selected region, with a 256 KiB limit.
The [build inspector](../data/examples/scripts/build-inspector.luau) finds compiler locations in command output.
The [creation example](../data/examples/scripts/create-and-title.luau) uses launch options and a returned pane ID.

## Diagnostics

`afterglow ctl scripts` reports `id`, `name`, `path`, `version`, `status`, `error`, `calls`, `handler_errors`, `effect_errors` and `last_error`.
`error` is the load error of a `failed` or `last-good` script, or why a script is `superseded`. Else it is empty.
`version` is the package version. A single-file script has none.
`status` is `active`, `disabled`, `last-good`, `failed` or `superseded`.
An error in a package module names the module file and its line.
Timing fields are `last_call_ms`, `max_call_ms` and `total_call_ms`.
Diagnostics also count what Afterglow dropped.
`dropped_callbacks` counts calls dropped at either handler call limit.
`dropped_effects` and `dropped_results` count dropped effects and results.
`waiting` is the number of handlers that wait for a result.
Timing measures VM handler execution, not asynchronous process duration.
Deferred failures count when the operation completes.
Unchanged instances retain counters. Replacements start new counters.

## Helpers

The [typed declarations](../afterglow/src/scripting/afterglow.d.luau) give the same signatures with comments.

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
| `afterglow.pulse` | `(pane: PaneRef, effect: ("portal" \| "flash" \| "shake" \| string \| number), options: { duration_ms: number?, color: number? }?) -> Effect` |
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
| `afterglow.exec` | `(name: string, command: string \| { string }, options: { env: {[string]: string \| false}?, cwd: string?, timeout_ms: number?, max_bytes: number? }?) -> Effect` |
| `afterglow.read_file` | `(name: string, path: string, options: { timeout_ms: number?, max_bytes: number? }?) -> Effect` |
| `afterglow.watch_file` | `(name: string, path: string) -> Effect` |
| `afterglow.set_commands` | `(commands: {[string]: CommandSpec}) -> Effect` |
| `afterglow.output` | `(run: string \| CommandEvent, text: string) -> Effect` |
| `afterglow.output_error` | `(run: string \| CommandEvent, text: string) -> Effect` |
| `afterglow.exit` | `(run: string \| CommandEvent, code: number) -> Effect` |
| `afterglow.select` | `(pane: PaneRef, title: string, items: { { id: string, title: string? } }) -> Effect` |
| `afterglow.prompt` | `(pane: PaneRef, title: string, text: string?) -> Effect` |
| `afterglow.overlay` | `(pane: PaneRef, command: string \| { string }, options: OverlayOptions?) -> Effect` |
| `afterglow.status` | `(pane: PaneRef, text: string?, tab_title: string?) -> Effect` |
| `afterglow.override` | `(pane: PaneRef, values: { cursorBlink: boolean?, backgroundOpacity: number?, unfocusedDim: number? }) -> Effect` |
| `afterglow.dock` | `(pane: PaneRef, spec: DockSpec) -> Effect` |
| `afterglow.undock` | `(pane: PaneRef, id: string, scope: ("window" \| "pane")?) -> Effect` |
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
| `afterglow.tab_menu` | `(pane: PaneRef) -> Effect` |
| `afterglow.move_tab` | `(pane: PaneRef, index: number) -> Effect` |
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
