<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# External control

Other programs can control Afterglow with the command line or the control socket.
They can list panes, send text, create panes, set titles, show notifications and run actions.
The `watch` command supplies events as JSON lines.

```sh
afterglow ctl --help
```

The control socket is a Unix socket that only your user can open.
It uses the [phosphor-ipc](#protocol) protocol: one JSON object on each line.
The calls and events are in the target `control`.

The application ID is `com.phosphorworks.Afterglow`.
Afterglow keeps D-Bus only for the desktop: launches, notifications and taskbar progress.

## Which process and pane

Every pane sets `AFTERGLOW_PANE_ID` to its pane ID and `AFTERGLOW_SOCKET` to the control socket of its Afterglow process.
In a pane, `afterglow ctl` sends its commands to that process.
Outside Afterglow, `afterglow ctl` uses the socket of the main process on the current Wayland display.

| Process | Socket |
| --- | --- |
| The main process, which owns `com.phosphorworks.Afterglow` | `$XDG_RUNTIME_DIR/afterglow-DISPLAY.sock` |
| A process started with `-o`, `--config` or `--class`, or another build | `$XDG_RUNTIME_DIR/afterglow-DISPLAY-PID.sock` |

`DISPLAY` is the name in `WAYLAND_DISPLAY`, `wayland-0` when it is not set.
`PID` is the process ID.
Only `afterglow ctl` in the panes of a separate process can reach that process.

On `SIGTERM`, `SIGINT` or `SIGHUP`, Afterglow closes its windows and its socket, as after the last window.
A second signal ends it at once.
A crash or `SIGKILL` leaves the socket file.
At start, Afterglow removes the `afterglow-*.sock` sockets in `$XDG_RUNTIME_DIR` that refuse a connection.

`afterglow ctl` selects the target pane in this order:

1. The pane that `--pane N` gives.
2. The pane in `AFTERGLOW_PANE_ID`.
3. The focused pane of the active window.
4. The focused pane of the window that was active last.

A terminal multiplexer such as tmux keeps the environment of the pane that started its server.
Inside tmux, `AFTERGLOW_PANE_ID` can then name a different pane, or a pane that is closed.
Use `--pane` inside a multiplexer, or let tmux update the variables when a client attaches:

```sh
set -ga update-environment AFTERGLOW_PANE_ID
set -ga update-environment AFTERGLOW_SOCKET
```

## Access settings

Set `Control.allowReadText=true` to permit external terminal text queries.
Set `Control.enabled=false` to close the control socket.
These settings are separate from [script text permissions](SCRIPTING.md#text-and-semantic-regions).

| Item | With `Control.allowReadText=false` |
| --- | --- |
| `getText` | Fails with an error. |
| `runAction` with `copy`, `paste`, `select_all` or `show_last_output` | Fails with an error. These actions move pane text to the clipboard, into another pane or into a pager. |
| The `command` field of `listPanes` | Empty. |
| The command in `commandStarted` and `commandFinished` | Empty. |
| The value in `userVarChanged` | Empty. |
| Titles in `listPanes` and `titleChanged` | Sent. A shell can put the running command in the title. |

`Control.allowReadText` does not apply to accessibility clients.
AT-SPI uses a separate bus.
While accessibility is active, any client on that bus can read the text in the viewport.
It can also run the [pane actions](DESKTOP.md#pane-actions) and press docked-area buttons. `Control.enabled` does not apply to them.

`Control.enabled=false` closes only the control socket.
The application object stays on D-Bus, because later launches of Afterglow use it.
Any program of the same user can call its `NewWindow` method, and that window can run a command.

## Protocol

A client writes one request on each line and reads one reply on each line.
Each request has an `id`, and the reply has the same `id`.

```json
{"type": "call", "id": 1, "target": "control", "fn": "split", "args": [3, "right", []]}
{"type": "reply", "id": 1, "result": {"pane": 4}}
```

| Request `type` | Fields | Reply |
| --- | --- | --- |
| `call` | `target`, `fn`, `args` | `result`: the call's object |
| `subscribe` | `target`, `signal`: an event name | `result`: `null`. Then an `event` line for each event, with `subscriptionId` (the request's `id`) and `args`. |
| `unsubscribe` | `subscriptionId` | `result`: `null` |
| `list` | None | `result`: the target names |
| `schema` | `target` | `result`: the calls and events of the target |

Every call replies with an object.
A call that fails replies with `{"error": "message"}` in `result`.
A request that the socket cannot run, such as an unknown call or a wrong argument count, gets a line with `"type": "error"`, a `code` and a `message`.

`phosphorctl` from the Phosphor shell can also use the socket: `phosphorctl --socket "$AFTERGLOW_SOCKET" list`.

## Application methods

These methods are on D-Bus, at the object path `/com/phosphorworks/Afterglow`.

| Method | Arguments | Result |
| --- | --- | --- |
| `NewWindow` | `s` working directory, `as` command, `a{sv}` options | None. Fails when the program cannot start. |
| `ToggleQuickTerminal` | None | None |

The `NewWindow` options are `directoryGiven` (`b`), `title` (`s`), `hold` (`b`) and `activationToken` (`s`).
An empty command runs the shell.

## Control calls

A pane argument is a pane ID.

| Call | Arguments | Result |
| --- | --- | --- |
| `listPanes` | None | `panes`: a [pane object](#pane-object) for each pane |
| `focusedPane` | None | `pane`: the pane that step 3 or 4 of the target order selects |
| `sendText` | pane, text | Empty |
| `getText` | pane, scope | `text`. Scopes are `screen`, `scrollback`, `selection` and `last-output`. |
| `split` | pane, direction, command | `pane`: the new pane. Directions are `left`, `right`, `up` and `down`. |
| `newTab` | pane, working directory, command | `pane`: the new tab's pane |
| `newWindow` | working directory, command | `pane`: the new window's pane |
| `focus` | pane | Empty |
| `close` | pane | Empty. The pane closes without a question, also when a program runs in it. |
| `setTitle` | pane, title | Empty. An empty title gives the title back to the programs. |
| `signal` | pane, channel, value | Empty. Channels are 0 to 7. |
| `notify` | pane, title, body | Empty |
| `runAction` | pane, action | Empty. Actions are keybind action names, or `script:ID.ACTION`. A keybind action runs in the pane's tab and shows it first. `close_tab`, `rename_tab`, `move_tab_left`, `move_tab_right` and `detach_tab` do not show it. `close_tab` asks first when a program runs in the tab and `Terminal.confirmClose` is on. The call returns before the answer. |
| `movePane` | pane, target, placement | The pane after the move. See [projects](PROJECTS.md). |
| `detachTab` | pane | The pane in its new window |
| `openDirectory` | directory, layout | The window and its pane |
| `projects` | None | `projects`: the saved project names |
| `saveProject` | pane, name, overwrite | The name and the window |
| `openProject` | name, new instance | The window and its panes |
| `scriptDiagnostics` | None | `diagnostics`: script and package diagnostics |

A command is an array of strings. An empty array runs the shell.
A working directory must be absolute, `~`, or start with `~/`.
For `newTab` and `newWindow`, an empty working directory uses the `Window.workingDirectory` setting.
`afterglow ctl` changes a relative directory into an absolute one before it sends it.

## Pane object

| Field | Content |
| --- | --- |
| `id` | The pane ID |
| `window` | The window ID |
| `tab` | The tab's position in its window, from 0 |
| `tab_id` | The tab ID. It does not change when the tab moves. |
| `title` | The pane title |
| `cwd` | The pane's working directory. Empty when it is unknown or removed. |
| `command` | The running command (shell integration), empty at the prompt |
| `cols` | Columns |
| `rows` | Rows |
| `focused` | True for the focused pane of the active window |
| `context` | Where the shell runs when it is not local as you, such as `root` or `deploy@prod-1`. Empty otherwise. |

## Events

| Event | Arguments |
| --- | --- |
| `paneCreated` | pane |
| `paneMoved` | pane. Sent when the pane moves to another split, tab or window, and for each pane of a tab that moves to another place in its window. |
| `paneClosed` | pane |
| `titleChanged` | pane, title |
| `bell` | pane |
| `commandStarted` | pane, command |
| `commandFinished` | pane, command, exit code, duration in milliseconds |
| `statusChanged` | pane, state, text. Sent when the state or the text changes. |
| `focusChanged` | pane |
| `userVarChanged` | pane, name, value |
| `contextChanged` | pane, context, such as `root` or `deploy@prod-1`. Sent when the context changes. |

The states of `statusChanged` are `idle`, `working`, `done`, `blocked` and `error`.
When a program clears its last status, or the terminal resets, the state is `idle` and the text is empty.
An empty context means that Afterglow knows of no remote, elevated or container context for the pane.

`Control.eventInterval` sets the shortest time between two state events of one pane.
The state events are `titleChanged`, `statusChanged`, `contextChanged`, `userVarChanged` (for each name) and `bell`.
A change in the interval waits, and a newer change replaces it.
The client then gets the newest value when the interval ends.
Before `paneClosed`, the client gets the changes of the pane that still wait.
The other events are never merged.
Set `Control.eventInterval=0` to send every change.

## Limits

| Item | Limit | At the limit |
| --- | --- | --- |
| Request line | 1 MiB | The socket closes the connection. `afterglow ctl send-text` sends long text in parts. |
| Waiting output to one client | 16 MiB | The socket closes the connection to a client that does not read its events. |
| Subscriptions on one connection | 256 | The subscription fails. |
| `setTitle` title | 256 UTF-16 units | The title is cut. |
| `notify` title | 256 UTF-16 units | The title is cut. |
| `notify` body | 2048 UTF-16 units | The body is cut. |

## Text escapes

`afterglow ctl send-text` changes these escapes, unless you give `--raw`:

| Escape | Character |
| --- | --- |
| `\n` | Line feed |
| `\r` | Carriage return |
| `\t` | Tab |
| `\e` | Escape |
| `\xHH` | The code point U+00HH, sent as UTF-8. Two hexadecimal digits are necessary. |

To send text that starts with `--`, put it after `--`: `afterglow ctl send-text -- --stdin`.

## Words after `--`

Only `send-text`, `set-title`, `notify`, `split`, `new-tab` and `new-window` read words after `--`.
The other commands stop with a usage error when you give such words.

## References

- [Pane movement and project commands](PROJECTS.md)
- [Shader signals and parameters](SHADERS.md)
- [Luau scripting](SCRIPTING.md)
- [All configuration keys](CONFIGURATION.md)

`afterglow --help` lists application launch options.
`afterglow --config-reference` prints the runtime configuration schema.
Use the option alone.
This command does not open a window or read user configuration.
