<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# External control

Other programs can control Afterglow with the command line or the control socket.
They can list panes, send text, create panes, set titles, show notifications and run actions.
The `watch` command writes each event as a JSON line, such as `{"args":[3,"vim"],"event":"titleChanged"}`.

```sh
afterglow ctl --help
```

Each Afterglow process listens on a Unix socket that only your user can open.
It uses the [phosphor-ipc](#protocol) protocol: one JSON object on each line.
The socket has two targets:

| Target | Use |
| --- | --- |
| `control` | The calls and events of external control, while `Control.enabled` is on |
| `launch` | A later launch of Afterglow hands its window to the main process |

Afterglow uses D-Bus only as a client of desktop services, such as notifications, taskbar progress and global shortcuts.
It owns no D-Bus name.

## Which process and pane

Every pane sets `AFTERGLOW_PANE_ID` to its pane ID and `AFTERGLOW_SOCKET` to the socket of its Afterglow process.
In a pane, `afterglow ctl` sends its commands to that process.
Outside Afterglow, `afterglow ctl` uses the socket of the main process on the current Wayland display.

| Process | Socket |
| --- | --- |
| The main process: the first to take the display's socket | `$XDG_RUNTIME_DIR/afterglow-DISPLAY.sock` |
| A process started with `-o`, `--config` or `--class`, another build, or a launch that cannot reach the main process | `$XDG_RUNTIME_DIR/afterglow-DISPLAY-PID.sock` |

`DISPLAY` is the value of `WAYLAND_DISPLAY`, or `wayland-0` when it is not set.
In an absolute `WAYLAND_DISPLAY`, the first `/` is removed.
Each character other than a letter, a digit, `-`, `_` and `.` becomes `_`.
`PID` is the process ID.

Outside its panes, `afterglow ctl` does not find a separate process.
A client reaches it with its socket path, for example `AFTERGLOW_SOCKET=PATH afterglow ctl ls`.

Without `XDG_RUNTIME_DIR`, Afterglow listens on no socket.
Each launch then starts its own process, and `afterglow ctl` stops with an error.
An Afterglow without a socket removes `AFTERGLOW_SOCKET` from its panes.
In such a pane, `afterglow ctl` stops with an error, because the pane ID means nothing to another process.

A process holds a lock on `PATH.lock` while it listens on the socket `PATH`.
The system releases the lock when the process ends, also after a crash or `SIGKILL`.
At start, Afterglow removes the `afterglow-*.sock` sockets in `$XDG_RUNTIME_DIR` that have no lock and refuse a connection.

On `SIGTERM`, `SIGINT` or `SIGHUP`, Afterglow closes its windows and its socket, as after the last window.
Then it ends with that termination signal, so that the program that started it sees the cause.
A second termination signal ends it at once.
A termination signal that Afterglow got as ignored, for example `SIGHUP` under `nohup`, stays ignored.

`afterglow ctl` selects the target pane in this order:

1. The pane that `--pane N` gives.
2. The pane in `AFTERGLOW_PANE_ID`.
3. The focused pane of the active window.
4. The focused pane of the window that was active last.

`ls`, `watch`, `scripts`, `open-directory`, `projects`, `open-project` and `new-window` act on no pane.
They stop with a usage error when you give `--pane`.

A terminal multiplexer such as tmux keeps the environment of the pane that started its server.
Inside tmux, `AFTERGLOW_PANE_ID` can then name a different pane, or a pane that is closed.
Use `--pane` inside a multiplexer, or let tmux update the variables when a client attaches:

```sh
set -ga update-environment AFTERGLOW_PANE_ID
set -ga update-environment AFTERGLOW_SOCKET
```

## Access settings

Set `Control.allowReadText=true` to permit external terminal text queries.
Set `Control.enabled=false` to remove the `control` target.
These settings are separate from [script text permissions](SCRIPTING.md#text-and-semantic-regions).

| Item | With `Control.allowReadText=false` |
| --- | --- |
| `getText` | Fails with an error. |
| `runAction` with `copy`, `paste`, `select_all` or `show_last_output` | Fails with an error. These actions move pane text to the clipboard, into another pane or into a pager. |
| The `command` field of `listPanes` | Empty. |
| The command in `commandStarted` and `commandFinished` | Empty. |
| The value in `userVarChanged` | Empty, also for a value that waited while the setting changed. |
| Titles in `listPanes` and `titleChanged` | Sent. A shell can put the running command in the title. |

These settings do not apply to accessibility clients.
See [terminal text](DESKTOP.md#terminal-text).

`Control.enabled=false` removes the `control` target, and ends its event subscriptions.
Each subscriber then gets an `error` line with the subscription's `id` and the code `NO_SUCH_TARGET`.
The socket stays open with the `launch` target, because later launches of Afterglow use it.
Any program of the same user can call `launch.newWindow`, and that window can run a command.

## Protocol

A client writes one request on each line and reads one reply on each line.
Give each request an `id` that is not 0 and that no other request on the connection uses.
The reply has the same `id`.

```json
{"type": "call", "id": 1, "target": "control", "fn": "split", "args": [3, "right", []]}
{"type": "reply", "id": 1, "result": 4}
{"type": "call", "id": 2, "target": "control", "fn": "focus", "args": [99]}
{"type": "error", "id": 2, "code": "CALL_FAILED", "message": "no pane 99"}
```

| Request `type` | Fields | Reply |
| --- | --- | --- |
| `call` | `target`, `fn`, `args` | `result`: the call's result, `null` for a call with no result |
| `subscribe` | `target`, `signal`: an event name | `result`: `null`. Then an `event` line for each event, with `subscriptionId` (the request's `id`) and `args`. |
| `unsubscribe` | `subscriptionId` | `result`: `null` |
| `list` | None | `result`: the target names |
| `schema` | `target` | `result`: the calls and events of the target |

A request that fails gets a line with `"type": "error"`, a `code` and a `message` in place of the reply.
An error for a line that does not parse has no `id`.

| Code | Cause |
| --- | --- |
| `CALL_FAILED` | The call ran and failed, for example for a pane that does not exist. The message tells why. |
| `NO_SUCH_TARGET` | The target is not there: `control` while `Control.enabled` is off. Also ends a subscription, with the subscription's `id`. |
| `NO_SUCH_FN` | No call has that name. |
| `NO_SUCH_SIGNAL` | No event has that name. |
| `NO_SUCH_SUBSCRIPTION` | `unsubscribe` names no subscription of this connection. |
| `INVALID_ARG` | The number or the type of the arguments is wrong. |
| `INVOCATION_FAILED` | The call could not run. |
| `MALFORMED_REQUEST` | The line is not a valid request, or a limit is reached. |

`phosphorctl` from the Phosphor shell can also use the socket: `phosphorctl --socket "$AFTERGLOW_SOCKET" list`.

## Launches

`afterglow` started again hands its window to the main process when that process runs the same executable.
All windows then share one GPU device.
The handoff uses the `launch` target of the display's socket.

1. The new process tries to take `afterglow-DISPLAY.sock`. Only one process can hold a path, so the first one becomes the main process.
2. When another process holds it, the new process reads that process's ID from the connection.
3. When that process runs the same executable file, the new process sends `launch.newWindow` and exits.
4. When it runs another file, such as another build, the new process starts on its own with `afterglow-DISPLAY-PID.sock`.

A launch while the main process starts waits until it is ready, for 120 seconds at most.
After that time, the launch starts on its own.
A launch also starts on its own when it cannot connect to the main process for 5 seconds.
It does the same when the executable of the main process cannot be read.
A launch whose command does not fit in a request of 1 MiB also starts on its own.
A main process that refuses the launch with another error stops it with that error.
A main process that quits stops answering, and a launch in that time takes the free path.
A process started with `-o`, `--config` or `--class` always starts on its own, as the table in [which process and pane](#which-process-and-pane) shows.

A window that a launch hands over runs with the environment of the main process, not of the launch.
Start Afterglow with `-o`, `--config` or `--class` when a window needs the environment of the launch.

| Call | Arguments | Result |
| --- | --- | --- |
| `newWindow` | working directory, command, options | None. Fails with `CALL_FAILED` when the program cannot start. |
| `toggleQuickTerminal` | None | None. Fails with `CALL_FAILED` when the quick terminal's shell cannot start. |

The `newWindow` options are `directoryGiven` (boolean), `title` (string), `hold` (boolean) and `activationToken` (string).
When `directoryGiven` is false or missing, the `Window.workingDirectory` setting can replace the working directory.
An empty command runs the shell.

## Control calls

A pane argument is a pane ID: a whole number from 0 to 4294967295.
Another number fails with `CALL_FAILED`.

| Call | Arguments | Result |
| --- | --- | --- |
| `listPanes` | None | A [pane object](#pane-object) for each pane |
| `focusedPane` | None | The pane that step 3 or 4 of the target order selects |
| `sendText` | pane, text | None |
| `getText` | pane, scope | The text. Scopes are `screen`, `scrollback`, `selection` and `last-output`. |
| `split` | pane, direction, command | The new pane. Directions are `left`, `right`, `up` and `down`. |
| `newTab` | pane, working directory, command | The new tab's pane |
| `newWindow` | working directory, command | The new window's pane |
| `focus` | pane | None |
| `close` | pane | None. The pane closes without a question, also when a program runs in it. |
| `setTitle` | pane, title | None. An empty title gives the title back to the programs. |
| `signal` | pane, channel, value | None. Channels are 0 to 7. |
| `notify` | pane, title, body | None |
| `runAction` | pane, action | None. Actions are keybind action names, or `script:ID.ACTION`. A keybind action runs in the pane's tab and shows it first. `close_tab`, `rename_tab`, `move_tab_left`, `move_tab_right` and `detach_tab` do not show it. `close_tab` asks first when a program runs in the tab and `Terminal.confirmClose` is on. The call returns before the answer. |
| `movePane` | pane, target, placement | An object: the pane after the move. See [projects](PROJECTS.md). |
| `detachTab` | pane | An object: the pane in its new window |
| `openDirectory` | directory, layout | An object: the window and its pane |
| `projects` | None | The saved project names |
| `saveProject` | pane, name, overwrite | An object: the name and the window |
| `openProject` | name, new instance | An object: the window and its panes |
| `scriptDiagnostics` | None | Script and package diagnostics |

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
| `paneMoved` | pane. Sent when the pane moves to another split, tab or window. Also sent for each pane of a tab that moves in its window. |
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

`Control.eventInterval` sets the shortest time in milliseconds between two events of one kind for one pane.
The default is 100, and the range is 0 to 10000.
These events are merged: `titleChanged`, `statusChanged`, `contextChanged`, `userVarChanged` (for each name) and `bell`.
An event that comes before the interval ends waits, and a newer event of the same kind replaces it.
Bells that wait become one `bell` event.
The client gets the event that waits when the interval ends.
Before any other event of the pane, such as `commandFinished` or `paneClosed`, the client gets the events of the pane that wait.
The other events are never merged.
Set `Control.eventInterval=0` to send every event.

## Limits

| Item | Limit | At the limit |
| --- | --- | --- |
| Request line | 1 MiB | The socket closes the connection. `afterglow ctl send-text` sends long text in parts. |
| `afterglow ctl send-text` text | 32 MiB of UTF-8 | `afterglow ctl` stops with an error and sends nothing. |
| Text that a program has not read | 32 MiB for each pane | `sendText` fails. Parts that `afterglow ctl send-text` sent before stay typed. |
| Lines that do not parse, one after the other | 16 | The socket closes the connection at the next one. |
| Waiting output to one client | 16 MiB | The socket closes the connection to a client that does not read its events. |
| Subscriptions on one connection | 256 | The subscription fails with `MALFORMED_REQUEST`. A second subscription to the same event also fails. |
| `afterglow ctl` reply time | 10 seconds | `afterglow ctl` stops with an error. A slow Afterglow can still do what it was asked. |
| `setTitle` title | 256 UTF-16 units | The title is cut. |
| `notify` title | 256 UTF-16 units | The title is cut. |
| `notify` body | 2048 UTF-16 units | The body is cut. |

`getText` has no size limit.
A reply with a long scrollback can take more than the 10 seconds of `afterglow ctl`.

Text that `afterglow ctl send-text` sends in parts arrives as separate inputs.
Keys and other clients can type between two parts.

## Text escapes

`afterglow ctl send-text` changes these escapes, unless you give `--raw` or `--stdin`:

| Escape | Character |
| --- | --- |
| `\n` | Line feed |
| `\r` | Carriage return |
| `\t` | Tab |
| `\e` | Escape |
| `\\` | Backslash |
| `\xHH` | The code point U+00HH, sent as UTF-8. Two hexadecimal digits are necessary. |

A backslash before another character is removed, and the character stays.
A `\x` without two hexadecimal digits stays as you typed it.
A backslash at the end of the text stays.

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
