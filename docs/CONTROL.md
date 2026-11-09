<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# External control

Other programs can control Afterglow with the command line or the control socket.
They can list panes, send text, create panes, set titles, show notifications and run actions.
They can also run a program over a pane (an [overlay](#overlays)), save a [screenshot](#screenshots) and run the commands of scripts.
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
In a pane, `afterglow ctl` and `aglow run` send their requests to that process.
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
The socket path has a limit of 107 bytes (see [limits](#limits)).

Outside its panes, `afterglow ctl` and `aglow run` do not find a separate process.
A client reaches it with its socket path, for example `AFTERGLOW_SOCKET=PATH afterglow ctl ls`.
`aglow run` refuses a socket on which a process of another user listens.
`afterglow ctl` does not do this check.

`afterglow ctl` and `aglow run` use the pane ID in `AFTERGLOW_PANE_ID` whenever `AFTERGLOW_SOCKET` is set.
They do not check that the pane belongs to the process of that socket.
In a pane, `AFTERGLOW_PANE_ID` names a pane of the process that runs the pane.
When you set `AFTERGLOW_SOCKET` to another Afterglow in a pane, clear `AFTERGLOW_PANE_ID` or give `--pane`.
If you do not, the command acts on the pane that has the same ID in the other process.

```sh
AFTERGLOW_SOCKET=PATH AFTERGLOW_PANE_ID= afterglow ctl action new_tab
```

Without `XDG_RUNTIME_DIR`, Afterglow listens on no socket.
A relative `XDG_RUNTIME_DIR` is not used.
Each launch then starts its own process, and `afterglow ctl` and `aglow run` stop with an error.
An Afterglow without a socket removes `AFTERGLOW_SOCKET` from its panes.
In such a pane, `afterglow ctl` and `aglow run` stop with an error, because the pane ID means nothing to another process.

A process holds a lock on `PATH.lock` while it listens on the socket `PATH`.
The system releases the lock when the process ends, also after a crash or `SIGKILL`.
At start, Afterglow removes the `afterglow-*.sock` sockets in `$XDG_RUNTIME_DIR` that have no lock and refuse a connection.

On `SIGTERM`, `SIGINT` or `SIGHUP`, Afterglow closes its windows and its socket, as after the last window.
Then it ends with that termination signal, so that the program that started it sees the cause.
A second termination signal ends it at once.
A termination signal that Afterglow got as ignored, for example `SIGHUP` under `nohup`, stays ignored.

`afterglow ctl` selects the target pane in this order:

1. The pane that `--pane N` gives. `N` is a pane ID from 1.
2. The pane in `AFTERGLOW_PANE_ID`, when `AFTERGLOW_SOCKET` is set.
3. The focused pane of the active window.
4. The focused pane of the window that was active last.

For `action script:ID.COMMAND`, `afterglow ctl` does steps 1 and 2.
When they give no pane, Afterglow does steps 3 and 4.
When no pane has the focus, the command runs with no pane, as with `aglow run`.

`ls`, `watch`, `scripts`, `open-directory`, `projects`, `open-project` and `new-window` act on no pane.
They stop with a usage error when you give `--pane`.

A terminal multiplexer such as tmux keeps the environment of the pane that started its server.
Inside tmux, `AFTERGLOW_PANE_ID` can then name a different pane, or a pane that is closed.
Use `--pane` inside a multiplexer, or let tmux update the variables when a client attaches.
To do this, add these lines to `~/.tmux.conf`. They are tmux commands, not shell commands:

```text
set -ga update-environment AFTERGLOW_PANE_ID
set -ga update-environment AFTERGLOW_SOCKET
```

tmux then gives the new values to the panes that start after the attach.
The tmux panes that exist already keep the old values. Use `--pane` in them.

## Access settings

Set `Control.allowReadText=true` to permit external terminal text queries and screenshots.
Set `Control.enabled=false` to remove the `control` and `commands` targets.
These settings are separate from [script text permissions](SCRIPTING.md#text-and-semantic-regions).

| Item | With `Control.allowReadText=false` |
| --- | --- |
| `getText` | Fails with an error. |
| `screenshot` | Fails with the code `denied`, also for a pane that does not exist. Only a pane ID that is not a whole number from 0 to 4294967295, or an unknown scope, fails first, with `CALL_FAILED`. A picture shows the text of the pane, and more. See [screenshots](#screenshots). |
| `runAction` with `copy`, `paste`, `select_all` or `show_last_output` | Fails with an error. These actions move pane text to the clipboard, into another pane or into a pager. |
| The `command` field of `listPanes` | Empty. |
| The command in `commandStarted` and `commandFinished` | Empty. |
| The value in `userVarChanged` | Empty, also for a value that waited while the setting changed. |
| The result of an `overlay` call | Has no `stdout`, also when the setting changed while the program ran. `exit_code` and `truncated` are sent. `afterglow ctl overlay` writes nothing to standard output. |
| Titles in `listPanes` and `titleChanged` | Sent. A shell can put the running command in the title. |
| The `cwd` and `context` fields of `listPanes`, the text of `statusChanged` and the context of `contextChanged` | Sent. Programs set them with escape sequences, as they set titles. |
| `read_text`, `read_region` and `semantic_regions` in a script command that `aglow run` or a `SCRIPT.COMMAND` call started | Fail with an error. The output of the command would take the text to the client. They also need `Scripts.allowReadText`. |

These settings do not apply to accessibility clients.
See [terminal text](DESKTOP.md#terminal-text).

`Control.enabled=false` removes the `control` and `commands` targets, and ends their event subscriptions.
Each subscriber then gets an `error` line with the subscription's `id` and the code `NO_SUCH_TARGET`.
The `run` calls that wait fail.
The overlays that control clients opened close, and their `overlay` calls fail with the code `cancelled`.
The sockets of the ssh sessions for [remote commands](#remote-commands) close, and their runs fail.
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
`overlay` of the `control` target also does this: it answers when its program exits.
`screenshot` of the `control` target also does this: it answers when the PNG of the frame is ready.
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
| `no_pane`, `busy`, `spawn_failed`, `cancelled` | An `overlay` call failed. See [overlays](#overlays). |
| `denied`, `busy`, `no_pane`, `not_shown`, `failed`, `cancelled` | A `screenshot` call failed. See [screenshots](#screenshots). |

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
| `screenshot` | pane, scope | An object: `png` (the PNG image in base64), `width` and `height` in device pixels. See [screenshots](#screenshots). |
| `overlayInput` | input name, text, end | `true`. Standard input for an `overlay` of the connection, in parts. See [overlays](#overlays). |
| `remoteCommandsEnabled` | None | `true` when `afterglow ssh` can forward a socket for [remote commands](#remote-commands) |
| `openRemoteSocket` | pane, process, host, directory, protocol, ssh options, destination, overlay | An object: `token`, `socket` and `path`, or `token` and `error`. See [remote commands](#remote-commands). |
| `remoteSocketForwarded` | token | None. `ssh -O forward` added the forward. Fails when the socket has closed. See [remote commands](#remote-commands). |
| `remoteSocketFailed` | token, reason | None. The reason is `forward`. |

A command is an array of strings. An empty array runs the shell.
A working directory must be absolute, `~`, or start with `~/`, and the directory must exist.
This rule also applies to the `cwd` option of `overlay`.
For `newTab` and `newWindow`, an empty working directory uses the `Window.workingDirectory` setting.
`afterglow ctl` changes a relative directory into an absolute one before it sends it.
When the current directory was removed, `afterglow ctl` refuses a relative directory and exits with status 1.

## Overlays

An overlay runs a program over a pane until the program exits, and returns its result.
The program shows in the place of the pane, or over a part of it as a popup, with the input focus.
[SCRIPTING.md](SCRIPTING.md#overlays) describes overlays and the options.

```sh
selected=$(git branch --format='%(refname:short)' | afterglow ctl overlay -w 80% -h 60% -- fzf) && git switch "$selected"
```

`afterglow ctl overlay [--capture stdout|file|none] [--cwd DIR] [--title TEXT] [-n|--no-stdin] [-w|--width N[%]] [-h|--height N[%]] [--position POS] [--border|--no-border] -- CMD...`
opens the overlay over the [target pane](#which-process-and-pane).
It waits until the program exits, with no time limit.
Then it writes the result's `stdout` to its standard output, and exits with the program's exit status.
A result over 1 MiB is cut, and a message goes to standard error.

| Option | Behavior |
| --- | --- |
| `--capture MODE` | `stdout` (the default), `file` or `none`, as the `capture` option |
| `--cwd DIR` | The program's directory. The default is the directory of `afterglow ctl`. The directory must exist. When the directory of `afterglow ctl` was removed, `afterglow ctl` exits with status 1. Then give an absolute `--cwd`, `~` or one that starts with `~/`. |
| `--title TEXT` | The pane's title while the overlay runs. It must not have a control character, a line break or a bidirectional control. |
| `-n`, `--no-stdin` | Standard input does not go to the program. |
| `-w`, `--width N` | A popup N cells wide, or `N%` of the pane's columns. The border is part of the width. N has no leading zero. |
| `-h`, `--height N` | A popup N cells tall, or `N%` of the pane's rows. The border is part of the height. N has no leading zero. |
| `--position POS` | Where the popup is: `center` (the default), `top`, `bottom`, `left`, `right`, `top-left`, `top-right`, `bottom-left` or `bottom-right`. It has no effect without `-w` or `-h`. |
| `--border`, `--no-border` | A border around the program, or none. A popup has a border by default, and a full-pane overlay has none. |

A width, a height or `--border` makes the overlay a popup: the pane stays visible around it.
`--border` alone makes a popup that fills the pane, with the program inside the border.
[SCRIPTING.md](SCRIPTING.md#popups) gives the limits and the layout.
`afterglow ctl` checks the options and the command before it reads standard input or connects.
A value that breaks a rule of [SCRIPTING.md](SCRIPTING.md#overlays) is a usage error, with exit status 2.
Afterglow checks that the `--cwd` directory exists. When it does not, `afterglow ctl` exits with status 1.

Without `-n`, standard input goes to the program when it is not a terminal.
The input can be up to 4 MiB of UTF-8, the limit of an overlay.
`afterglow ctl` sends it in parts with `overlayInput`, before the `overlay` call.
`afterglow ctl` reads standard input before it connects. It refuses input that is not UTF-8 text.
When standard input is a terminal, the program reads the pane.

When `afterglow ctl` stops before the program exits, for example by a signal, its connection closes.
The overlay then closes, and its program gets `SIGHUP`.
When `afterglow ctl` runs in the target pane, the overlay takes the keyboard of that pane.
Ctrl+C then goes to the program of the overlay, not to `afterglow ctl`.
The `cancel_overlay` action closes the overlay of the focused pane.
`afterglow ctl overlay` then exits with status 1.

While an overlay runs, `sendText` and `getText` act on the pane under it, not on the program of the overlay.
Thus `afterglow ctl send-text` types into the pane under the overlay.
A `screenshot` shows the overlay, as the window shows it.

The `overlay` call takes the pane (0 for the focused pane), the argv list and an object of options.

| Option | Type |
| --- | --- |
| `stdin`, `capture`, `cwd`, `title` | string |
| `env` | object |
| `width`, `height` | a JSON integer of cells, or a string such as `"80%"` |
| `position` | string |
| `border` | boolean |

In place of `stdin`, the option `input` names standard input that came before the call in `overlayInput` calls.
Give the parts in order, and `true` as the end with the last part.
The name is 1 to 64 letters, digits, `_` and `-`, and belongs to the connection.
The `overlay` call takes the input, also when it fails, for example for a pane ID that is not a whole number. An input that has not ended fails the call.
Afterglow drops the inputs of a connection when it closes, and all the inputs when `Control.enabled` goes off.

The `overlay` call answers when the program exits: the reply comes later, and other calls on the connection get their replies meanwhile.
An argv list or an option that breaks a rule fails the call with `CALL_FAILED`, and no program starts.
An empty `cwd` starts the program in the home directory.
Other failures have their own error codes:

| Code | Cause |
| --- | --- |
| `no_pane` | The pane does not exist, or the pane is 0 and no pane has the focus. |
| `busy` | The pane has an overlay already. Or, in its window, a script dialog, a prompt, a confirmation, the command palette or a context menu is open. Or a script's key mode holds the keyboard. |
| `spawn_failed` | The program cannot start. |
| `cancelled` | The overlay closed before the program exited. For example, its pane closed, its client went away, or `Control.enabled` went off. |

## Screenshots

`afterglow ctl screenshot [--tab|--window] FILE` saves a PNG image of the [target pane](#which-process-and-pane), its tab or its window, as the window shows it.
The image includes the shader packs, and an overlay or a popup over the pane.
It also includes the chrome over its area, for example the scrollbar, a prompt, the palette or a menu.
Only `--window` includes the tab bar.
A translucent background stays translucent in the image. What is behind the window is not in the image.

| Scope | Option | The image |
| --- | --- | --- |
| `pane` | None | The pane with its padding and its docked areas. The title of the pane shows when `Window.paneTitles` is on and the tab has two or more panes. |
| `tab` | `--tab` | The panes of the pane's tab: the window without the tab bar and without the docked areas of the window |
| `window` | `--window` | The whole window. It shows the tab that the window shows, which can be another tab than the tab of the pane. |

Give `-` as `FILE` to write the image to standard output.
`afterglow ctl` refuses a terminal, as `-` or as a `FILE` such as `/dev/tty`, and exits with status 2.
It also refuses a socket as `FILE`, such as `/dev/stdout` when standard output is a socket, and exits with status 2.
`afterglow ctl` writes a new file beside `FILE`, then puts it in the place of `FILE`.
Thus it writes all of the image or nothing, and when the write fails, an existing `FILE` stays as it was.
In a directory where `afterglow ctl` cannot make a new file, it writes over an existing `FILE`.
It writes directly to a pipe or a device, such as `/dev/fd/3`, too.
In these two cases, a failed write can leave part of the image.

Afterglow draws a frame for the screenshot and reads it back. The size is in device pixels.
During a tab switch, the `pane` and `tab` scopes wait for the end of the switch.
While a pane moves to its place, for example after a split, the `pane` scope waits until it is there.
Afterglow must draw the frame of a screenshot in 2 seconds. Each frame of such a wait starts these 2 seconds again.
A wait has no limit of its own. `afterglow ctl` stops after 30 seconds.
The `screenshot` call takes the pane (0 for the focused pane) and the scope: `pane`, `tab` or `window`.
Another scope fails the call with `CALL_FAILED`.
The reply is `{"png": BASE64, "width": W, "height": H}`.

The `screenshot` call needs `Control.allowReadText`.
A screenshot shows more than `getText` reads, for example the program of an overlay, the text of a paste confirmation and the dialogs of scripts.

| Code | Cause |
| --- | --- |
| `denied` | `Control.allowReadText` is off. Afterglow checks it again when the reply goes out. |
| `busy` | 4 screenshots are in progress, from all clients. |
| `no_pane` | No pane has the ID, or no pane has the focus for a pane of 0. |
| `not_shown` | The window is not on the screen, is minimized, or closed before the frame. For the `pane` and `tab` scopes also: the pane is in a tab in the background, the pane closed, or the overview shows or closes. For the `pane` scope also: another pane of the tab is zoomed. |
| `failed` | Afterglow drew no frame in 2 seconds, for example when the compositor sends no frames to the window. It also fails when Afterglow cannot read the frame or write the PNG. Some GPU drivers cannot read a window back: Afterglow then logs a warning. |
| `cancelled` | `Control.enabled` is off when the image is ready. |

The reply of a screenshot of a large window can be larger than 16 MiB.
The socket closes a connection when more than 16 MiB of output waits and an event comes.
Thus take screenshots on a connection without subscriptions.

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
| `paneMoved` | pane. Sent when the pane moves to another split, tab or window. Also sent for each pane of a tab that moves in its window, and of each tab that the move puts at another index. |
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

`aglow run` runs a command of a script and passes its output through.
It exits with the command's exit status.
`afterglow run` is the same command: it runs the `aglow` next to `afterglow`.
When it cannot run that `aglow`, it exits with status 127 or 126, as [the aglow client](DESKTOP.md#the-aglow-client) tells.
See [commands](SCRIPTING.md#commands) for the script side.

### aglow run

| Command | Result |
| --- | --- |
| `aglow run` | Lists the commands |
| `aglow run --help` | Shows the usage |
| `aglow run ID --help` | Lists the commands of one script |
| `aglow run [-n] [--pane N] [--] ID COMMAND [ARGS...]` | Runs a command |
| `aglow run --complete [ID]` | Prints the names for shell completion |
| `aglow run --complete --all` | Prints all the commands for shell completion |

| Option | Effect |
| --- | --- |
| `-n`, `--no-stdin` | Does not pass standard input to the command. |
| `--pane N` | The pane that the command acts on. `N` is a pane ID from 1. |
| `--` | Ends the options. The next word is the script ID. |

The words after `COMMAND` go to the command, options too.
The command acts on the pane of `--pane N`, else on the pane in `AFTERGLOW_PANE_ID`, else on the focused pane.
When no pane has the focus, the command runs with no pane.
`aglow run` finds its Afterglow and uses `AFTERGLOW_PANE_ID` as `afterglow ctl` does (see [which process and pane](#which-process-and-pane)).
When `AFTERGLOW_SOCKET` is not set, or names no socket of yours on this computer, `aglow run` first asks the terminal for the socket of an ssh session.
It asks only when standard output is its terminal and it runs in the foreground of that terminal.
It waits at most 2 seconds for the answer.
See [remote commands](#remote-commands).

`aglow run` reads standard input only for a command with `stdin`, and only when standard input is not a terminal.
Thus a command that you type at a prompt does not wait for input.
When standard input is not a terminal and does not end, for example under `ssh host cmd`, use `-n`.
`aglow run` refuses standard input that is not UTF-8 text, and exits with status 2.

Before it reads standard input, `aglow run` checks that the script and the command exist.
It sends standard input before the run.
Afterglow checks the pane and the directory only after the input ends.
Thus a run with a wrong `--pane` reads all its standard input before it fails.
`aglow run` waits for the answers to its standard input and for the run without a time limit, because a slow ssh link can take long to carry them.
`Ctrl+C` ends the wait, and so does a connection that closes.
Over ssh, a link that stops with no close ends the wait only when ssh finds the link dead.
ssh finds it at its keepalives (`ServerAliveInterval` in `ssh_config`), or after the TCP keepalive time of the system.
For a run that no person watches, set a time limit with `timeout(1)`, for example `timeout 60 aglow run tools build`.

| Exit status | Cause |
| --- | --- |
| 0 to 255 | The exit status of the command. |
| 1 | An error, for example no Afterglow, commands that are off or a pane that does not exist. Also no list of commands in 10 seconds, for the query to the terminal, the connection and the list together. A failed handler, a run over a limit and a working directory that is not UTF-8 also give status 1. |
| 2 | A usage error, or a script or command that does not exist. Arguments that do not fit in one request of 1 MiB also give status 2. So do more than 256 arguments, an argument over 4096 UTF-16 units or not in UTF-8, and a working directory over 4096 UTF-16 units. Standard input that is not UTF-8 text or is over `Scripts.commandStdinLimit` also gives status 2. |
| 124 | The run took longer than `Scripts.commandTimeout`. |
| 126 | Only `afterglow run`: the `aglow` next to `afterglow` cannot run. |
| 127 | Only `afterglow run`: there is no `aglow` next to `afterglow`, the build has no `aglow`, or `afterglow` cannot find its own executable. |
| 130 | `SIGINT`. `aglow run` cancels the run, then ends by the signal. A shell reports 130. |
| 141 | Standard output closed (`SIGPIPE`). `aglow run` cancels the run, then ends by the signal. A shell reports 141. |
| 143 | `SIGTERM`. `aglow run` cancels the run, then ends by the signal. A shell reports 143. |

A command can also exit with 1, 2, 124, 130, 141 or 143.
The exit status alone does not tell the cause.
Because `aglow run` ends by the signal, a shell loop of runs stops at `Ctrl+C`.
A `SIGINT` or `SIGTERM` that `aglow run` got as ignored stays ignored.
For example, a shell script runs `aglow run ID COMMAND &` as a background job, and the job gets `SIGINT` as ignored.
Then `Ctrl+C` does not cancel that run.
When `aglow run` got `SIGPIPE` as ignored, a closed standard output ends it with exit status 141, not by the signal.

### Shell completion

`aglow run --complete` prints the script IDs that have commands, one on each line.
`aglow run --complete ID` prints `COMMAND<TAB>SUMMARY` lines for the commands of the script `ID`.
`aglow run --complete --all` prints `ID<TAB>COMMAND<TAB>SUMMARY` lines for the commands of all scripts, from one connection.
The lines come in the order of the list of commands.
The output leaves out a script or command whose name has a character other than a letter, a digit, `_` and `-`.
A summary shows `?` in place of each control character, such as a C0, C1 or bidirectional control.
`aglow run --complete` waits at most 1 second for Afterglow, for the query to the terminal, the connection and the list together.
It prints no error message.
A shell's completion reads the output through a pipe, so `aglow run --complete` does not ask the terminal there.
On a remote host, set `AFTERGLOW_SOCKET` to the socket of the ssh session to complete the remote commands.

| Exit status | Cause |
| --- | --- |
| 0 | `aglow run --complete` printed the names. When it finds no command, it prints nothing. |
| 1 | No Afterglow, no answer in 1 second, or commands off. |
| 2 | More than one word after `--complete`. |

The shell completions for Bash, Zsh and Fish use `aglow run --complete`, for `aglow run` and for `afterglow run`.
The completions of `afterglow ctl action` get the `script:ID.COMMAND` actions from `aglow run --complete --all`.
The completion of `afterglow` uses the `aglow` that `afterglow run` runs.
`aglow` starts faster than `afterglow`, because it does not load Qt.
The completion of `afterglow` uses the completion of `aglow` for `run`, `transfer`, `dnd`, `icat` and `clipboard`.
In a build without `aglow`, the completions offer no script commands. For these five commands, they complete file names.
The Fish completions need Fish 4 or later.
[Install](BUILD.md#install) gives the paths of the completion files, and the Zsh directory for Debian and Ubuntu.

### The commands target

| Call | Arguments | Result |
| --- | --- | --- |
| `list` | None | The commands: objects with `script`, `command`, `summary`, `usage`, `palette`, `stdin` and `remote`. |
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
Afterglow keeps the input of a run ID until `run` starts the run, `cancel` drops it, or the connection closes.
A `run` that fails before the run starts keeps the input. Examples are a pane that does not exist and a directory that is not absolute.
A `run` whose input has not ended also fails and keeps the input, because the command would take a part for all of it.
The client can then send `run` again with the same run ID, or call `cancel`.
A `run` that cannot start the command drops the input, for example for a command that does not exist or a script at its `Scripts.commandRunLimit`.
Afterglow does not cut the input, because a command could not tell that it got only a part.
An `input` that takes the input of a run over `Scripts.commandStdinLimit` fails with `INPUT_TOO_LARGE`.
Afterglow then drops all the input of that run.
Input of exactly the limit is accepted.
A limit of 0 refuses any input that is not empty.
A command without `stdin` gets no input, also when the client sent some.

Afterglow keeps the input as UTF-8.
All the input that waits for `run`, of all connections together, has a limit of 64 MiB.
The connections of the sockets of the ssh sessions count toward the same limit.
Thus a remote host can fill it, and an `input` from this computer then fails until that input goes.
An `input` over that limit fails, and the input that waits stays.

When a connection closes, Afterglow cancels its runs and drops its input.
`Control.enabled=false` or `Scripts.enabled=false` removes the target, and the `run` calls that wait fail.

### Remote commands

A command that a script marks `remote` can also run from a remote host.
Run `aglow run` there, in a session of `afterglow ssh`.
The run acts on the pane of the ssh session, and its `ev.source` is `remote`.
The handler still gets all the panes in `ev.terminal`, and its effects can act on any pane, as for a local run.
`Terminal.sshRemoteCommands` is off by default.
With it on, every process of the remote user, and the remote root user, can run the commands that are marked `remote`.

Remote commands need all of these:

- `Terminal.sshRemoteCommands`, `Control.enabled` and `Scripts.enabled`. `Terminal.sshIntegration` does not count: you can also run `afterglow ssh` yourself.
- `AFTERGLOW_SOCKET` and `AFTERGLOW_PANE_ID` in the environment of `afterglow ssh`, as a pane of Afterglow sets them. `afterglow ssh` calls that socket for the pane.
- A terminal on the remote host: no remote command on the ssh command line, or `-t`.
- A shared ssh connection. `afterglow ssh` makes one in `XDG_RUNTIME_DIR`, or uses the `ControlPath` of your SSH configuration. `XDG_RUNTIME_DIR` must be an absolute path of letters, digits, `/`, `.`, `_` and `-`, which ssh reads as a path and nothing more. Afterglow does not check that only you can enter it: keep it private, as systemd makes it.
- No `ControlPath none` and no `ControlMaster no`, on the command line or in your SSH configuration. Afterglow keeps that choice and forwards no socket for the session.
- With your own `ControlPath` and a `ControlMaster` other than `auto` or `autoask`: a master that runs already.
- With `-M` on the command line: your own `ControlPath`, and a master that runs already on it. `-M` sets `ControlMaster yes`, and ssh with `-M` does not join a master. The forward goes on the master that runs, and the session authenticates on its own. A master that `afterglow ssh` made would hold the forward with no session, and end after its `ControlPersist`.
- No tmux around `afterglow ssh` on this computer. In tmux, `AFTERGLOW_PANE_ID` can name another pane.
- `aglow` on the remote host, with the protocol of this Afterglow. `aglow --version` shows the protocol.
- `aglow` on the `PATH` that the login shell of the remote user gets without a terminal. The probe below runs there.
- `AllowStreamLocalForwarding` on the ssh server. Its default is `yes`.

When one of these is missing, `afterglow ssh` forwards no socket and prints nothing, with two exceptions.
With `-v`, `afterglow ssh` prints why it forwards no socket. Without `AFTERGLOW_SOCKET` or `AFTERGLOW_PANE_ID`, it prints nothing, also with `-v`.
When the remote `aglow` has another protocol, `afterglow ssh` prints one line on standard error.
It prints that line once for each host and version, and records them in `$XDG_STATE_HOME/afterglow/ssh-remote`, by default under `~/.local/state`.
The file holds only hashes, at most 512. To see the line again, delete the file.

Before the session starts, `afterglow ssh` runs one probe over the shared connection:

1. The probe reports that it runs on the remote host.
2. The probe runs `aglow --version`.
3. When `aglow` is there, the probe makes the remote directory with mode 0700.
4. The probe runs `aglow remote-prune` in that directory.

The probe runs in the login shell of the ssh server, so `aglow` must be on the `PATH` that the shell gets without a terminal.
A probe that ran is not a failure of ssh, whatever the exit status of `aglow remote-prune`.

`aglow remote-prune DIR` removes the sockets in `DIR` whose names end with `.sock`, that belong to you and that no process holds.
The ssh server leaves the socket of a session that ended, and the next probe removes it.
On Linux, a socket that `/proc/net/unix` lists stays, also one that the ssh server has bound and does not listen on yet.
Elsewhere, and for a socket that the list does not name, only a refused connection tells that the socket is dead.
Files that are not sockets, have another name or belong to another user stay.

| Remote directory | When |
| --- | --- |
| `$XDG_RUNTIME_DIR/afterglow` | `XDG_RUNTIME_DIR` is an absolute path on the remote host |
| `~/.local/state/afterglow/run/HOST` | Else, when `HOME` is an absolute path. `HOST` is the output of `uname -n`. Many hosts can share a home on NFS, and a socket is live only on its own host |
| None | Else. Afterglow forwards no socket |

The socket on the remote host is `DIRECTORY/TOKEN.sock`. The token is 16 hexadecimal characters.
When this path is longer than 103 bytes, Afterglow adds no forward.
On the BSDs and macOS, a socket path holds 104 bytes with the NUL at its end.
Afterglow listens on `$XDG_RUNTIME_DIR/afterglow-remote-TOKEN.sock`, with mode 0600, and holds the lock file `afterglow-remote-TOKEN.sock.lock`.
`afterglow ssh` then runs `ssh -F none -S CONTROLPATH -O forward -R REMOTE:LOCAL` on the shared connection.
`CONTROLPATH` is the expanded `ControlPath`. Without your configuration and your command line, the command adds and cancels no other forward.
The session itself never gets `-R`.
The pane gives the path to `aglow run` only after the forward is there.
A remote directory with `:` or `\`, or a local runtime directory with one, gets no socket, because ssh splits `-R` at `:`.

The local socket closes when the `afterglow ssh` process ends, when its pane closes or when a setting turns remote commands off.
Its runs end. Each client gets the reply to its run, an error that gives the reason, and then the connection closes.
Afterglow gives each connection 5 seconds to take what waits for it. A client that does not read loses what does not fit in its socket, and the connection closes.
Afterglow then runs `ssh -O cancel` with the same options and the same `-R` value.
When the forward comes after the socket closed, `afterglow ssh` cancels it itself.
When the shared connection has already gone, there is nothing to cancel.
A pane has at most 16 sessions, and Afterglow at most 256.
The socket of a session takes connections from every process of the remote user, at most 64 at the same time.
Each `aglow run` uses one connection. Afterglow closes a connection over the limit before it reads a request, and logs a warning.
`aglow run` then fails with a message that starts with `lost Afterglow`.

The socket has one target, `remote`.
It has the calls `list`, `input`, `run`, `ack` and `cancel` of [the commands target](#the-commands-target), and the same events.
`list` and `run` know only the commands that are marked `remote`. A `run` of another command fails with `NO_SUCH_COMMAND`.
A `run` acts on the pane of the ssh session. A pane of 0 is that pane, and another pane fails.
The directory of a `run` is a directory on the remote host. The handler gets it as text.
The socket has no `control` target and no `SCRIPT.COMMAND` calls.

`aglow run` asks the terminal for the socket with [OSC 24569](COMPATIBILITY.md#remote-commands-osc-24569).
It asks only when standard output is its terminal and its process group is in the foreground of that terminal.
When its output goes into a pipeline, such as `aglow run ID COMMAND | less`, another program can read the terminal and take the answer.
A background job would stop when it changes the modes of the terminal.
In these cases, and without a terminal, `aglow run` asks nothing.
A program that writes into `aglow run` through a pipe and reads the terminal can still take the answer.
When it then finds no Afterglow, its message tells how to reach one.
Set `AFTERGLOW_SOCKET` to the socket of the ssh session, `DIRECTORY/TOKEN.sock`.
`aglow run` uses a socket with such a name as the socket of an ssh session, with the target `remote`.

While `aglow run` waits for the answer, the terminal does not echo and does not edit lines.
The wait ends at the answer to the guard query, or after 2 seconds.
Inside tmux, `aglow run` first asks tmux for `allow-passthrough`, in the first half of the wait. When tmux answers later, `aglow run` does not ask the terminal.
`aglow run` reads all the input up to the answer. Keys that you type before the answer are lost, also keys that you typed before `aglow run` started.
Without the answer, `aglow run` also discards the input and output of the terminal that wait. Thus a late answer does not go to the shell.
`Ctrl+C`, `Ctrl+Z`, `Ctrl+\` and other signals act after `aglow run` restores the modes of the terminal.
`Ctrl+C` ends the wait at the answer to the guard query. Then `aglow run` ends by `SIGINT`.

When the terminal gives a path of a socket of yours on this computer, `aglow run` uses that socket.
Else it uses the socket of an Afterglow on this computer, when there is one.
When there is none, `aglow run` exits with status 1.
Its message on standard error starts with `aglow run: `, or `afterglow run: ` for `afterglow run`. One of these texts follows.
`PATH` is the path that the terminal gave.

| Text in the message | Cause |
| --- | --- |
| `the terminal names no socket of an ssh session` | A requirement above is missing, or the pane runs no ssh session. The text comes after the reason why there is no Afterglow on this computer |
| `the socket of the commands has no forward` | The server has `AllowStreamLocalForwarding no`. Or Afterglow did not confirm the forward in 2 seconds, and `afterglow ssh` cancelled it. `afterglow ssh -v` tells which |
| `this aglow speaks another protocol than the Afterglow of this ssh session` | The `aglow` on the remote host and Afterglow have different protocols |
| `the path of the socket on this host would be over 103 bytes` | The remote directory is too long |
| `the socket of this ssh session, PATH, is on another host` | `aglow run` runs on a host after a second ssh hop |
| `cannot use the socket of this ssh session, PATH` | The path is not a socket, or the socket belongs to another user |
| `inside tmux, aglow cannot ask the terminal for the socket of an ssh session` | `aglow run` runs in tmux, and `allow-passthrough` is off. Run `tmux set -g allow-passthrough on`. The text comes after the reason why there is no Afterglow on this computer |

When `aglow run` has a socket of an ssh session and cannot use it, it also exits with status 1:

| Text in the message | Cause |
| --- | --- |
| `the socket PATH has no target for remote commands` | The path is a socket, but not one that `afterglow ssh` forwarded |
| `the socket of this ssh session does not answer` | The forward is there, but the shared ssh connection behind it has gone |

On the socket of an ssh session, `aglow run` with no arguments lists only the commands that are marked `remote`.

`afterglow ssh` uses these calls of the `control` target:

| Call | Use |
| --- | --- |
| `remoteCommandsEnabled` | Before the probe: whether the settings allow remote commands |
| `openRemoteSocket` | After the probe. The table below gives the arguments. The result has `token`, `socket` and `path`. With another protocol or a path over 103 bytes, it has `token` and `error` (`version` or `length`), and no socket. |
| `remoteSocketForwarded` | `ssh -O forward` added the forward. The pane answers with the path. The call fails when the socket has closed, and `afterglow ssh` then cancels the forward |
| `remoteSocketFailed` | `ssh -O forward` failed, or `afterglow ssh` cancelled the forward because `remoteSocketForwarded` got no answer in 2 seconds. Afterglow closes the socket, and the pane answers `error=forward` |

These are the arguments of `openRemoteSocket`, in their order:

| Argument | Value | The call fails when |
| --- | --- | --- |
| pane | The pane of the ssh session | The pane does not exist, or it has 16 sessions |
| process | The process ID of `afterglow ssh`. The socket closes when this process ends | It is not a whole number from 1, or the process does not run |
| host | The host name for `ev.remote.host` | It is empty, has more than 255 UTF-16 units or has a control character |
| directory | The remote directory | It is not an absolute path, or it has `:`, `\` or a control character |
| protocol | The protocol of the remote `aglow` | It is not a whole number, or it is larger than 10^9 or smaller than -10^9. Another protocol gives the `error` `version` |
| ssh options | The options for `ssh -O cancel`: `-F none -S CONTROLPATH` | There are more than 256 |
| destination | The ssh destination | It is empty, or it starts with `-` |
| overlay | `true` when `afterglow ssh` runs in the pane's overlay (`AFTERGLOW_OVERLAY` is `1`). The overlay's terminal then answers OSC 24569 for the session, and the pane keeps the answer of its own session | — |

The call also fails when Afterglow has 256 sessions, when `XDG_RUNTIME_DIR` is not set, or when the local socket path would be over 107 bytes.
`afterglow ssh` then forwards no socket.

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
The runs of the sockets of the ssh sessions count toward the same limit, so a slow remote client can fill it for the local runs too.
A run whose output would go past it ends with exit status 1, and its standard error tells why.

A call of a `SCRIPT.COMMAND` entry gets its output without flow control. Its client does not send `ack`.

## Limits

| Item | Limit | At the limit |
| --- | --- | --- |
| Socket path | 107 bytes | Afterglow cannot listen on a longer path. It logs a warning and runs with no socket, as without `XDG_RUNTIME_DIR`. `afterglow ctl` and `aglow run` stop with exit status 1. |
| Socket path of an ssh session on the remote host, `DIRECTORY/TOKEN.sock` | 103 bytes | Afterglow forwards no socket for the session. The pane answers `error=length`. |
| Socket path of an ssh session on this computer | 107 bytes. The name `/afterglow-remote-TOKEN.sock` adds 39 bytes, so `XDG_RUNTIME_DIR` can have at most 68 | `openRemoteSocket` fails, and `afterglow ssh` forwards no socket. |
| Sessions of remote commands | 16 for each pane, 256 in Afterglow | `openRemoteSocket` fails, and `afterglow ssh` forwards no socket. |
| Connections to the socket of one ssh session | 64 at the same time | Afterglow closes the new connection before it reads a request, and logs a warning once until a connection closes. `aglow run` fails with exit status 1. |
| Request line | 1 MiB | The socket closes the connection. `afterglow ctl send-text` sends long text in parts. |
| `afterglow ctl send-text` text | 32 MiB of UTF-8 | `afterglow ctl` exits with status 2 and sends nothing. It checks the text before it connects. |
| Text that a program has not read | 32 MiB for each pane | `sendText` fails. Parts that `afterglow ctl send-text` sent before stay typed. |
| Lines that do not parse, one after the other | 16 | The socket closes the connection at the next one. |
| Waiting output to one client | 16 MiB | The socket closes the connection to a client that does not read its events. |
| Subscriptions on one connection | 256 | The subscription fails with `MALFORMED_REQUEST`. A second subscription to the same event also fails. |
| `afterglow ctl` reply time | 10 seconds, 30 seconds for `screenshot` | `afterglow ctl` stops with an error. A slow Afterglow can still do what it was asked. `afterglow ctl overlay` waits for the program without a limit. |
| Time to draw the frame of a screenshot | 2 seconds | The `screenshot` call fails with the code `failed`. |
| Screenshots in progress | 4, for all clients | The `screenshot` call fails with the code `busy`. |
| `afterglow ctl overlay` standard input | 4 MiB | `afterglow ctl` exits with status 2 and opens nothing. It checks the input before it connects. |
| One part of `overlayInput` from `afterglow ctl` | 512 KiB, written as a JSON string | `afterglow ctl` cuts the input there. JSON writes most C0 control characters, such as U+0001, as 6 bytes, and a part with them holds fewer bytes of input. |
| Inputs of `overlayInput` that wait for `overlay` | 4 for each connection, 64 MiB for all connections | The `overlayInput` call fails. At the limit of 64 MiB, the parts that came before stay and wait. They go when `overlay` takes them, the connection closes or `Control.enabled` goes off. An input over 4 MiB fails, and Afterglow drops it. |
| `afterglow ctl overlay --title` | 512 UTF-16 units | `afterglow ctl` exits with status 2 before it connects. |
| `stdin` of the `overlay` call | The request line of 1 MiB | The socket closes the connection. |
| `stdout` of an overlay's result | 1 MiB of UTF-8 | The text is cut, and `truncated` is true. |
| `aglow run` time to get the list of commands | 10 seconds | `aglow run` stops with exit status 1. The query to the terminal, the connection and `list` share this time. `aglow run` waits for the replies of `input` and for the end of a run without a limit. It does not wait for the reply of `cancel` or `ack`. |
| `input` part that `aglow run` sends | 512 KiB of JSON string | `aglow run` sends longer input in parts. |
| Runs whose input waits for `run`, on one connection | 16 | `input` fails. |
| Input that waits for `run`, in all connections | 64 MiB of UTF-8 | `input` fails. |
| Standard input of one run | `Scripts.commandStdinLimit` | `input` fails with `INPUT_TOO_LARGE`. `aglow run` exits with status 2. |
| Output of the runs of one connection that the client did not acknowledge | 4 MiB of event lines, and one line more | The rest of the output waits in Afterglow. The output of a run that ended does not count. See [flow control](#flow-control). |
| Output that waits for clients, in all runs | 256 MiB in memory | The run whose output would go past it ends with exit status 1. |
| Arguments of `aglow run` | One request of 1 MiB | `aglow run` exits with status 2 before it connects. |
| Arguments of one run | 256 | `aglow run` exits with status 2 before it connects. A `run` call with more fails with `CALL_FAILED`. |
| One argument of a run | 4096 UTF-16 units | `aglow run` exits with status 2 before it connects. A `run` call with a longer argument fails with `CALL_FAILED`. |
| The directory of a run | 4096 UTF-16 units | `aglow run` exits with status 2 before it connects. A `run` call with a longer directory fails with `CALL_FAILED`. |
| `run` calls that wait, on one connection | 256 | `run` fails with `CALL_FAILED`. |
| `setTitle` title | 256 UTF-16 units | The title is cut. |
| `notify` title | 256 UTF-16 units | The title is cut. |
| `notify` body | 2048 UTF-16 units | The body is cut. |

`getText` and `screenshot` have no size limit.
A reply with a long scrollback can take more than the 10 seconds of `afterglow ctl`.
A screenshot of a large window also takes time: Afterglow waits for a frame and encodes the PNG.

Text that `afterglow ctl send-text` sends in parts arrives as separate inputs.
Keys and other clients can type between two parts.

`afterglow ctl send-text --stdin` types standard input as it is.
It refuses standard input that is not UTF-8 text. It then exits with status 2 and sends nothing.

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

The word after an option that takes a value, such as `--pane`, `--cwd` or `--title`, is that value.
It is that value also when it starts with `-`, but not when it is `--`.
Give a `FILE` or a `PATH` that starts with `--` as `./--FILE` or `./--PATH`.

## Exit status of afterglow ctl

| Exit status | Cause |
| --- | --- |
| 0 | The command succeeded. `afterglow ctl --help` also exits with status 0. |
| 1 | An error, for example no Afterglow, a call that failed or no reply in time. |
| 2 | A usage error, for example an unknown command, a missing argument or an option that the command does not take. An option given two times is a usage error too. So is an `overlay` option or command that breaks a rule. `afterglow ctl` with no command also gives status 2. So does refused text for `send-text`: over 32 MiB, or standard input that is not UTF-8 text. So does standard input for `overlay` over 4 MiB or not UTF-8 text. So does `screenshot` when its `FILE`, or `-`, is a terminal, or when `FILE` is a socket. `afterglow ctl` finds these errors before it connects. |
| 0 to 255 from `overlay` | `afterglow ctl overlay` exits with the exit status of its program. Thus a status of 1 or 2 can come from the program. A failed or cancelled overlay gives 1. So does an exit status outside 0 to 255. |

`afterglow ctl watch` prints events until the connection or a subscription ends.
Then it exits with status 1.

## References

- [Pane movement and project commands](PROJECTS.md)
- [Shader signals and parameters](SHADERS.md)
- [Luau scripting](SCRIPTING.md)
- [All configuration keys](CONFIGURATION.md)

`afterglow --help` lists application launch options.
`afterglow --config-reference` prints the runtime configuration schema.
Use the option alone.
This command does not open a window or read user configuration.
