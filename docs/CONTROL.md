<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# External control

Other programs can control Afterglow with the command line or D-Bus.
They can list panes, send text, create panes, set titles, show notifications and run actions.
The `watch` command supplies events as JSON lines.

```sh
afterglow ctl --help
```

The application and D-Bus service ID is `com.phosphorworks.Afterglow`.
The application object path is `/com/phosphorworks/Afterglow`.
The control interface is `com.phosphorworks.Afterglow.Control`.
Its object path is `/com/phosphorworks/Afterglow/Control`.

## Which process and pane

Every pane sets `AFTERGLOW_PANE_ID` to its pane ID and `AFTERGLOW_BUS_NAME` to the D-Bus name of its Afterglow process.
In a pane, `afterglow ctl` sends its commands to that process.
Outside Afterglow, `afterglow ctl` sends its commands to the process that owns `com.phosphorworks.Afterglow`.
A process started with `-o`, `--config` or `--class` does not own that name.
Only `afterglow ctl` in the panes of that process can reach it.

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
set -ga update-environment AFTERGLOW_BUS_NAME
```

## Access settings

Set `Control.allowReadText=true` to permit external terminal text queries.
Set `Control.enabled=false` to disable the control interface.
These settings are separate from [script text permissions](SCRIPTING.md#text-and-semantic-regions).

| Item | With `Control.allowReadText=false` |
| --- | --- |
| `GetText` | Fails with an error. |
| `RunAction` with `copy`, `paste`, `select_all` or `show_last_output` | Fails with an error. These actions move pane text to the clipboard, into another pane or into a pager. |
| The `command` field of `ListPanes` | Empty. |
| The command in `CommandStarted` and `CommandFinished` | Empty. |
| The value in `UserVarChanged` | Empty. |
| Titles in `ListPanes` and `TitleChanged` | Sent. A shell can put the running command in the title. |

`Control.allowReadText` does not apply to accessibility clients.
AT-SPI uses a separate bus.
While accessibility is active, any client on that bus can read the text in the viewport.

`Control.enabled=false` removes only the control interface.
The application object stays on the bus, because later launches of Afterglow use it.
Any program of the same user can call its `NewWindow` method, and that window can run a command.

## Application methods

| Method | Arguments | Result |
| --- | --- | --- |
| `NewWindow` | `s` working directory, `as` command, `a{sv}` options | None. Fails when the program cannot start. |
| `ToggleQuickTerminal` | None | None |

The `NewWindow` options are `directoryGiven` (`b`), `title` (`s`), `hold` (`b`) and `activationToken` (`s`).
An empty command runs the shell.

## Control methods

Every method that fails replies with the error `org.freedesktop.DBus.Error.Failed` and a message.
A pane argument is a pane ID.

| Method | Arguments | Result |
| --- | --- | --- |
| `ListPanes` | None | `a(uuisssiib)`: one `PaneInfo` for each pane |
| `FocusedPane` | None | `u`: the pane that step 3 or 4 of the target order selects |
| `SendText` | `u` pane, `s` text | None |
| `GetText` | `u` pane, `s` scope | `s`: the text. Scopes are `screen`, `scrollback`, `selection` and `last-output`. |
| `Split` | `u` pane, `s` direction, `as` command | `u`: the new pane. Directions are `left`, `right`, `up` and `down`. |
| `NewTab` | `u` pane, `s` working directory, `as` command | `u`: the new tab's pane |
| `CreateWindow` | `s` working directory, `as` command | `u`: the new window's pane |
| `Focus` | `u` pane | None |
| `Close` | `u` pane | None. The pane closes without a question, also when a program runs in it. |
| `SetTitle` | `u` pane, `s` title | None. An empty title gives the title back to the programs. |
| `Signal` | `u` pane, `i` channel, `d` value | None. Channels are 0 to 7. |
| `Notify` | `u` pane, `s` title, `s` body | None |
| `RunAction` | `u` pane, `s` action | None. Actions are keybind action names, or `script:ID.ACTION`. |
| `MovePane` | `u` pane, `u` target, `s` placement | `a{sv}`: the pane after the move. See [projects](PROJECTS.md). |
| `DetachTab` | `u` pane | `a{sv}`: the pane in its new window |
| `OpenDirectory` | `s` directory, `s` layout | `a{sv}`: the window and its pane |
| `Projects` | None | `as`: the saved project names |
| `SaveProject` | `u` pane, `s` name, `b` overwrite | `a{sv}`: the name and the window |
| `OpenProject` | `s` name, `b` new instance | `a{sv}`: the window and its panes |
| `ScriptDiagnostics` | None | `s`: script and package diagnostics as JSON |

A working directory must be absolute, `~`, or start with `~/`.
For `NewTab` and `CreateWindow`, an empty working directory uses the `Window.workingDirectory` setting.
`afterglow ctl` changes a relative directory into an absolute one before it sends it.

## PaneInfo

| Field | Type | Content |
| --- | --- | --- |
| `id` | `u` | The pane ID |
| `window` | `u` | The window ID |
| `tab` | `i` | The tab's position in its window, from 0 |
| `title` | `s` | The pane title |
| `cwd` | `s` | The pane's working directory. Empty when it is unknown or removed. |
| `command` | `s` | The running command (shell integration), empty at the prompt |
| `cols` | `i` | Columns |
| `rows` | `i` | Rows |
| `focused` | `b` | True for the focused pane of the active window |

## Signals

| Signal | Arguments |
| --- | --- |
| `PaneCreated` | `u` pane |
| `PaneMoved` | `u` pane |
| `PaneClosed` | `u` pane |
| `TitleChanged` | `u` pane, `s` title |
| `Bell` | `u` pane |
| `CommandStarted` | `u` pane, `s` command |
| `CommandFinished` | `u` pane, `s` command, `i` exit code, `x` duration in milliseconds |
| `StatusChanged` | `u` pane, `s` state, `s` text. Sent when the state or the text changes. |
| `FocusChanged` | `u` pane |
| `UserVarChanged` | `u` pane, `s` name, `s` value |
| `ContextChanged` | `u` pane, `s` context, such as `root` or `deploy@prod-1` |

The states of `StatusChanged` are `idle`, `working`, `done`, `blocked` and `error`.

## Limits

| Item | Limit | At the limit |
| --- | --- | --- |
| `GetText` reply | 96 MiB of UTF-8 text | The call fails. Use a smaller scope. |
| `SetTitle` title | 256 UTF-16 units | The title is cut. |
| `Notify` title | 256 UTF-16 units | The title is cut. |
| `Notify` body | 2048 UTF-16 units | The body is cut. |

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
