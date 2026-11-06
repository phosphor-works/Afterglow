<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# External control

Other programs can control Afterglow with the command line or the control socket.
They can list panes, send text, create panes, set titles, show notifications, run actions and run the commands of scripts.
The `watch` command writes each event as a JSON line, such as `{"args":[3,"vim"],"event":"titleChanged"}`.

```sh
afterglow ctl --help
```

Each Afterglow process listens on a Unix socket that only your user can open.
It uses the [phosphor-ipc](#protocol) protocol: one JSON object on each line.
The socket has three targets:

| Target | Use |
| --- | --- |
| `control` | The calls and events of external control, while `Control.enabled` is on |
| `commands` | The [commands of scripts](#script-commands), while `Control.enabled` and `Scripts.enabled` are on |
| `launch` | A later launch of Afterglow hands its window to the main process |

Afterglow uses D-Bus only as a client of desktop services, such as notifications, taskbar progress and global shortcuts.
It owns no D-Bus name.

## Which process and pane

Every pane sets `AFTERGLOW_PANE_ID` to its pane ID and `AFTERGLOW_SOCKET` to the socket of its Afterglow process.
In a pane, `afterglow ctl` and `afterglow run` send their requests to that process.
Outside Afterglow, they use the socket of the main process on the current Wayland display.

| Process | Socket |
| --- | --- |
| The main process: the first to take the display's socket | `$XDG_RUNTIME_DIR/afterglow-DISPLAY.sock` |
| A process started with `-o`, `--config` or `--class`, another build, or a launch that cannot reach the main process | `$XDG_RUNTIME_DIR/afterglow-DISPLAY-PID.sock` |

`DISPLAY` is the value of `WAYLAND_DISPLAY`, or `wayland-0` when it is not set.
A `WAYLAND_DISPLAY` in `$XDG_RUNTIME_DIR` loses that directory first, so both spellings name one socket.
In another absolute `WAYLAND_DISPLAY`, the first `/` is removed.
Each byte other than a letter, a digit, `-`, `_` and `.` becomes `_`.
`PID` is the process ID.

Outside its panes, `afterglow ctl` and `afterglow run` do not find a separate process.
A client reaches it with its socket path, for example `AFTERGLOW_SOCKET=PATH afterglow ctl ls`.
`afterglow run` refuses a socket of another user.

Without `XDG_RUNTIME_DIR`, Afterglow listens on no socket.
A relative `XDG_RUNTIME_DIR` is not used.
Each launch then starts its own process, and `afterglow ctl` and `afterglow run` stop with an error.
An Afterglow without a socket removes `AFTERGLOW_SOCKET` from its panes.
In such a pane, `afterglow ctl` and `afterglow run` stop with an error, because the pane ID means nothing to another process.

A process holds a lock on `PATH.lock` while it listens on the socket `PATH`.
The system releases the lock when the process ends, also after a crash or `SIGKILL`.
At start, Afterglow removes the `afterglow-*.sock` sockets in `$XDG_RUNTIME_DIR` that have no lock and refuse a connection.

On `SIGTERM`, `SIGINT` or `SIGHUP`, Afterglow closes its windows and its socket, as after the last window.
Then it ends with that termination signal, so that the program that started it sees the cause.
A second termination signal ends it at once.
A termination signal that Afterglow got as ignored, for example `SIGHUP` under `nohup`, stays ignored.

`afterglow ctl` selects the target pane in this order:

1. The pane that `--pane N` gives. `N` is a pane ID from 1.
2. The pane in `AFTERGLOW_PANE_ID`, when `AFTERGLOW_SOCKET` is the socket of that pane's process.
3. The focused pane of the active window.
4. The focused pane of the window that was active last.

For `action script:ID.COMMAND`, Afterglow does steps 3 and 4.
When no pane has the focus, the command runs with no pane, as with `afterglow run`.

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
Set `Control.enabled=false` to remove the `control` and `commands` targets.
These settings are separate from [script text permissions](SCRIPTING.md#text-and-semantic-regions).

| Item | With `Control.allowReadText=false` |
| --- | --- |
| `getText` | Fails with an error. |
| `runAction` with `copy`, `paste`, `select_all` or `show_last_output` | Fails with an error. These actions move pane text to the clipboard, into another pane or into a pager. |
| The `command` field of `listPanes` | Empty. |
| The command in `commandStarted` and `commandFinished` | Empty. |
| The value in `userVarChanged` | Empty, also for a value that waited while the setting changed. |
| Titles in `listPanes` and `titleChanged` | Sent. A shell can put the running command in the title. |
| `read_text`, `read_region` and `semantic_regions` in a script command that `afterglow run` or a `SCRIPT.COMMAND` call started | Fail with an error. The output of the command would take the text to the client. They also need `Scripts.allowReadText`. |

These settings do not apply to accessibility clients.
See [terminal text](DESKTOP.md#terminal-text).

`Control.enabled=false` removes the `control` and `commands` targets, and ends their event subscriptions.
Each subscriber then gets an `error` line with the subscription's `id` and the code `NO_SUCH_TARGET`.
The `run` calls that wait fail.
The socket stays open with the `launch` target, because later launches of Afterglow use it.
Any program of the same user can call `launch.newWindow`, and that window can run a command.

## Protocol

A client writes one request on each line and reads one reply on each line.
Each line is one JSON object in UTF-8, and ends with a line feed (`\n`).
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

A call can answer after other calls on the connection, when its target answers later.
`run` of the `commands` target does this: it answers when the run ends.
Use the `id` to find the reply of each request.

An event goes to a subscriber with `subscriptionId`.
An event for one connection only has `target` and `signal` in place of `subscriptionId`.
`afterglow ctl watch` prints only the events of its subscriptions.
The output of a script command comes in this form:

```json
{"type": "event", "target": "commands", "signal": "stdout", "args": ["5f0c...", "hello\n"]}
```

Everything that the socket writes to a connection arrives in the order it was written.
The events of a run arrive before the reply of the run.

| Code | Cause |
| --- | --- |
| `CALL_FAILED` | The call ran and failed, for example for a pane that does not exist. The message tells why. |
| `CALL_ABANDONED` | The target could not answer a call that it answers later. |
| `NO_SUCH_COMMAND` | `run` names a script or a command that does not exist. |
| `INPUT_TOO_LARGE` | `input` takes the standard input of a run over `Scripts.commandStdinLimit`. |
| `NO_SUCH_TARGET` | The target is not there: `control` while `Control.enabled` is off, or `commands` while `Control.enabled` or `Scripts.enabled` is off. Also ends a subscription, with the subscription's `id`. |
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
| `runAction` | pane, action | None. Actions are keybind action names, or `script:ID.COMMAND`, which starts a [script command](#script-commands) with no arguments and returns before it ends. A keybind action runs in the pane's tab and shows it first. `close_tab`, `rename_tab`, `move_tab_left`, `move_tab_right` and `detach_tab` do not show it. `close_tab` asks first when a program runs in the tab and `Terminal.confirmClose` is on. The call returns before the answer. A keybind action fails while its window shows a prompt or a script dialog, so that it cannot change the answer. For `script:ID.COMMAND`, a pane of 0 is the focused pane. When no pane has the focus, the command runs with no pane. |
| `movePane` | pane, target, placement | An object: the pane after the move. See [projects](PROJECTS.md). |
| `detachTab` | pane | An object: the pane in its new window |
| `openDirectory` | directory, layout | An object: the window and its pane |
| `projects` | None | The saved project names |
| `saveProject` | pane, name, overwrite | An object: the name and the window |
| `openProject` | name, new instance | An object: the window and its panes |
| `scriptDiagnostics` | None | Script and package diagnostics |
| `overlay` | pane, argv, options | An object: `exit_code`, `stdout` and `truncated`. See [overlays](#overlays). |

A command is an array of strings. An empty array runs the shell.
A working directory must be absolute, `~`, or start with `~/`.
For `newTab` and `newWindow`, an empty working directory uses the `Window.workingDirectory` setting.
`afterglow ctl` changes a relative directory into an absolute one before it sends it.

## Overlays

An overlay runs a program over a pane until the program exits, and returns its result.
The program shows in the place of the pane, with the input focus.
[SCRIPTING.md](SCRIPTING.md#overlays) describes overlays and the options.

```sh
selected=$(git branch --format='%(refname:short)' | afterglow ctl overlay -w 80% -h 60% -- fzf) && git switch "$selected"
```

`afterglow ctl overlay [--capture stdout|file|none] [--cwd DIR] [--title TEXT] [-n] [-w N|N%] [-h N|N%] [--position POS] [--border|--no-border] -- CMD...`
opens the overlay over the [target pane](#which-process-and-pane).
It waits until the program exits, with no time limit.
Then it writes the result's `stdout` to its standard output, and exits with the program's exit status.
A result over 1 MiB is cut, and a message goes to standard error.

| Option | Behavior |
| --- | --- |
| `--capture` | `stdout` (the default), `file` or `none`, as the `capture` option |
| `--cwd DIR` | The program's directory. The default is the directory of `afterglow ctl`. |
| `--title TEXT` | The pane's title while the overlay runs |
| `-n`, `--no-stdin` | Standard input does not go to the program. |
| `-w`, `--width N` | A popup N cells wide, or `N%` of the pane's columns. The border is part of the width. |
| `-h`, `--height N` | A popup N cells high, or `N%` of the pane's rows. The border is part of the height. |
| `--position POS` | Where the popup is: `center` (the default), `top`, `bottom`, `left`, `right`, `top-left`, `top-right`, `bottom-left` or `bottom-right` |
| `--border`, `--no-border` | A border around the overlay, or none. A popup has a border by default, and a full-pane overlay has none. |

A width or a height makes the overlay a popup: the pane stays visible around it.
[SCRIPTING.md](SCRIPTING.md#popups) gives the limits and the layout.

Without `-n`, standard input goes to the program when it is not a terminal.
The input goes in the request, so it is at most 768 KiB. For a longer list, use a script.
When standard input is a terminal, the program reads the pane.
When `afterglow ctl` stops before the program exits (Ctrl+C, a signal), its connection closes.
The overlay then closes, and its program gets `SIGHUP`.

The `overlay` call takes the pane (0 for the focused pane), the argv list and an object of options: `stdin`, `capture`, `cwd`, `env`, `title`,
`width`, `height`, `position` and `border`. Give `width` and `height` as a JSON integer of cells, or as a string such as `"80%"`.
It answers when the program exits: the reply comes later, and other calls on the connection get their replies meanwhile.
An option that breaks a rule fails the call with `CALL_FAILED`.
Other failures have their own error codes:

| Code | Cause |
| --- | --- |
| `no_pane` | The pane does not exist. |
| `busy` | The pane has an overlay already, or a script dialog is open in its window. |
| `spawn_failed` | The program cannot start. |
| `cancelled` | The overlay closed before the program exited, for example because its pane closed. |

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

## Script commands

`afterglow run` runs a command of a script and passes its output through.
It exits with the command's exit status.
See [commands](SCRIPTING.md#commands) for the script side.

### afterglow run

```sh
afterglow run                                         # list the commands
afterglow run --help                                  # the usage
afterglow run ID --help                               # the commands of one script
afterglow run [-n] [--pane N] [--] ID COMMAND [ARGS…]   # run a command
afterglow run --complete [ID]                         # the names for shell completion
```

| Option | Effect |
| --- | --- |
| `-n`, `--no-stdin` | Does not pass standard input to the command. |
| `--pane N` | The pane that the command acts on. `N` is a pane ID from 1. |
| `--` | Ends the options. The next word is the script ID. |

The words after `COMMAND` go to the command, options too.
The command acts on the pane of `--pane N`, else on `AFTERGLOW_PANE_ID`, else on the focused pane.
`afterglow run` finds its Afterglow as `afterglow ctl` does (see [which process and pane](#which-process-and-pane)).

`afterglow run` reads standard input only for a command with `stdin`, and only when standard input is not a terminal.
Thus a command that you type at a prompt does not wait for input.
When standard input is not a terminal and does not end, for example under `ssh host cmd`, use `-n`.
`afterglow run` sends standard input before the run.
Afterglow checks the pane, the directory and the command only after the input ends.
Thus a run with a wrong `--pane` reads all its standard input before it fails.

| Exit status | Cause |
| --- | --- |
| 0 to 255 | The exit status of the command. |
| 1 | An error: no Afterglow, commands off, a failed handler, a run over a limit, or a working directory that is not UTF-8. |
| 2 | A usage error, a script or command that does not exist, or arguments that do not fit in one request of 1 MiB. Also more than 256 arguments, an argument over 4096 UTF-16 units or not in UTF-8, or standard input over `Scripts.commandStdinLimit`. |
| 124 | The run took longer than `Scripts.commandTimeout`. |
| 130 | `SIGINT`. `afterglow run` cancels the run, then ends by the signal. A shell reports 130. |
| 141 | Standard output closed (`SIGPIPE`). `afterglow run` cancels the run, then ends by the signal. A shell reports 141. |
| 143 | `SIGTERM`. `afterglow run` cancels the run, then ends by the signal. A shell reports 143. |

A command can also exit with 1, 2, 124, 130, 141 or 143.
The exit status alone does not tell the cause.
Because `afterglow run` ends by the signal, a shell loop of runs stops at `Ctrl+C`.
A `SIGINT` or `SIGTERM` that `afterglow run` got as ignored, for example under `nohup`, stays ignored.

`afterglow run --complete` prints the script IDs that have commands, one on each line.
`afterglow run --complete ID` prints `COMMAND<TAB>SUMMARY` lines.
It prints only names of letters, digits, `_` and `-`.
It waits at most 1 second for Afterglow, for the connection and the list together.
The shell completions for bash, zsh and fish use them.
They are in `share/bash-completion/completions/afterglow`, `share/zsh/site-functions/_afterglow` and `share/fish/vendor_completions.d/afterglow.fish`.
The fish completion needs Fish 4 or later.
[BUILD.md](BUILD.md) gives the zsh directory for Debian and Ubuntu.

### The commands target

| Call | Arguments | Result |
| --- | --- | --- |
| `list` | None | The commands: objects with `script`, `command`, `summary`, `usage`, `palette` and `stdin`. |
| `input` | run, text, end | `true`. Send the standard input before `run`, in parts. Input over `Scripts.commandStdinLimit` fails with `INPUT_TOO_LARGE`. |
| `run` | run, script, command, arguments, directory, pane | `{"exit": STATUS}` when the run ends and its output has gone. A pane of 0 is the focused pane. The directory is absolute, or empty for the pane's directory. |
| `ack` | run, bytes | None. The client wrote these bytes of the run's event lines. Afterglow accepts no more than the run sent. See [flow control](#flow-control). |
| `cancel` | run | None. See [cancel](#cancel). |
| `SCRIPT.COMMAND` | arguments | `{"exit": STATUS}` when the run ends. Runs the command with the arguments and no standard input. |

| Event | Arguments |
| --- | --- |
| `stdout` | run, text |
| `stderr` | run, text |

The events go only to the connection that called `run`. They have no `subscriptionId`.
A subscription to them gets nothing.

Each command has a `SCRIPT.COMMAND` call, for example `projects.list`, so that the schema lists the commands.
Such a run acts on the focused pane, or on no pane when no pane has the focus.
Its run ID is Afterglow's, and the source of the run is `control`.
Its output has no flow control.

The client makes up the run ID, for example 16 random bytes in hexadecimal.
A run ID is 1 to 64 letters, digits, `_` and `-`. It belongs to one connection.
A `run` with an ID that a running run of the connection uses fails.

The standard input is complete before the handler starts, because the handler gets it as one string.
Afterglow keeps the input of a run ID until `run` comes, `cancel` drops it, or the connection closes.
Afterglow does not cut the input, because a command could not tell that it got only a part.
An `input` that takes the input of a run over `Scripts.commandStdinLimit` fails with `INPUT_TOO_LARGE`.
Afterglow then drops all the input of that run.
Input of exactly the limit is accepted.
A limit of 0 refuses any input.
A command without `stdin` gets no input, also when the client sent some.

Afterglow keeps the input as UTF-8.
All the input that waits for `run`, of all connections together, has a limit of 64 MiB.
An `input` over that limit fails, and the input that waits stays.

When a connection closes, Afterglow cancels its runs and drops its input.
`Control.enabled=false` or `Scripts.enabled=false` removes the target, and the `run` calls that wait fail.

### Cancel

| The run | What `cancel` does |
| --- | --- |
| Runs | Drops the output that waits, and ends the run. The `run` reply has exit status 130. |
| Has ended, and its output waits | Drops that output. The `run` reply has the exit status of the run. |
| Has not started | Drops its standard input. |
| Is not a run of the connection | Fails with `CALL_FAILED`. |

### Flow control

The socket closes a connection that has more than 16 MiB of output waiting for it.
To keep a slow client connected, Afterglow sends the output of a `run` only a window ahead of what the client acknowledges:

1. The client reads an event line of a run.
2. The client writes the text to its output.
3. The client adds the size of the line in bytes, with its line feed, to the count of that run.
4. When the counts of all the runs of the connection are 256 KiB or more together, do steps 5 and 6.
5. For each run with a count, the client calls `ack` with the run and its count.
6. The client sets the count of each run to 0.

Afterglow sends output while the bytes that the client did not acknowledge are less than 4 MiB.
This window is for all the runs of a connection together.
One line more can go past 4 MiB, because Afterglow sends a line while less than 4 MiB waits.
The rest waits in Afterglow, and the script's wait for `afterglow.output` waits with it.
The output that waits counts toward `Scripts.commandOutputLimit`.
The reply of `run` comes after all the output of the run.

The client acknowledges the total of its runs, not each run alone.
Many runs of less than 256 KiB each can fill the window, and an `ack` for each run alone would never come.
Afterglow takes no more bytes in an `ack` than the run sent.

At the end of a run, its bytes no longer count toward the window, also when the client did not read them yet.
Thus the window keeps a slow client below 16 MiB for one run at a time.
A slow client with many runs on one connection can still go past 16 MiB, and the socket then closes it.
An `ack` for a run that has ended fails, and the client can ignore that error.

All the output that waits for clients, of all runs together, has a limit of 256 MiB in memory.
A run whose output would go past it ends with exit status 1, and its standard error tells why.

A call of a `SCRIPT.COMMAND` entry gets its output without flow control. Its client does not send `ack`.

## Limits

| Item | Limit | At the limit |
| --- | --- | --- |
| Request line | 1 MiB | The socket closes the connection. `afterglow ctl send-text` sends long text in parts. |
| `afterglow ctl send-text` text | 32 MiB of UTF-8 | `afterglow ctl` stops with an error and sends nothing. |
| Text that a program has not read | 32 MiB for each pane | `sendText` fails. Parts that `afterglow ctl send-text` sent before stay typed. |
| Lines that do not parse, one after the other | 16 | The socket closes the connection at the next one. |
| Waiting output to one client | 16 MiB | The socket closes the connection to a client that does not read its events. |
| Subscriptions on one connection | 256 | The subscription fails with `MALFORMED_REQUEST`. A second subscription to the same event also fails. |
| `afterglow ctl` reply time | 10 seconds | `afterglow ctl` stops with an error. A slow Afterglow can still do what it was asked. `afterglow ctl overlay` waits for the program without a limit. |
| `afterglow ctl overlay` standard input | 768 KiB | `afterglow ctl` stops with an error and opens nothing. |
| `stdin` of the `overlay` call | 16 MiB of UTF-8, and one request line of 1 MiB | The call fails. |
| `stdout` of an overlay's result | 1 MiB of UTF-8 | The text is cut, and `truncated` is true. |
| `afterglow run` reply time for `list` and `input` | 10 seconds | `afterglow run` stops with exit status 1. The time to connect counts toward the time of `list`. It does not wait for the reply of `cancel` or `ack`. It waits for the end of a run without a limit. |
| `input` part that `afterglow run` sends | 512 KiB of JSON string | `afterglow run` sends longer input in parts. |
| Runs whose input waits for `run`, on one connection | 16 | `input` fails. |
| Input that waits for `run`, in all connections | 64 MiB of UTF-8 | `input` fails. |
| Standard input of one run | `Scripts.commandStdinLimit` | `input` fails with `INPUT_TOO_LARGE`. `afterglow run` exits with status 2. |
| Output of the runs of one connection that the client did not acknowledge | 4 MiB of event lines, and one line more | The rest of the output waits in Afterglow. The output of a run that ended does not count. See [flow control](#flow-control). |
| Output that waits for clients, in all runs | 256 MiB in memory | The run whose output would go past it ends with exit status 1. |
| Arguments of `afterglow run` | One request of 1 MiB | `afterglow run` exits with status 2. |
| `run` calls that wait, on one connection | 256 | `run` fails with `CALL_FAILED`. |
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

Only `send-text`, `set-title`, `notify`, `split`, `new-tab`, `new-window` and `overlay` read words after `--`.
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
