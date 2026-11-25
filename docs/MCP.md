<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# The MCP server

`afterglow mcp` is a server of the Model Context Protocol (MCP) for agents, such as Claude Code.
It gives the agent a [mux session](MUX.md) of its own.
The panes of the session run with no window.
The agent opens panes, runs commands, sends keys to interactive programs, and reads the text of the panes.
It also opens and saves your [projects](PROJECTS.md), and runs the [commands of your scripts](SCRIPTING.md#commands).
You can watch the session, or take it over, with `afterglow mux attach NAME`.

The server speaks MCP revision 2026-07-28 on its standard input and output.
It refuses requests of other revisions, and the `initialize` request of older revisions.
It writes its messages for people on standard error.

## Add the server to a client

The MCP client starts the server, and ends it when the agent is done.

For Claude Code, run this command in the directory of your project:

```sh
claude mcp add afterglow -- afterglow mcp
```

For another client, add the server to its configuration:

```json
{
  "mcpServers": {
    "afterglow": {
      "command": "afterglow",
      "args": ["mcp"]
    }
  }
}
```

The first pane of the session starts in the working directory of the server.
The client sets that directory, usually to the directory of the project.

## Command line

```
afterglow mcp [--session NAME] [--adopt] [--cwd DIR]
```

| Option | Function |
| --- | --- |
| `--session NAME` | Use session `NAME`, or create it. Without this option, the server creates a session with a new name. |
| `--adopt` | With `--session`: use `NAME` also when it is not an agent session, and make it one. The server never ends that session. The session needs `Control.allowAdopt`, also an agent session that an agent started. |
| `--cwd DIR` | Start the first pane of a new session in `DIR`. The default is the working directory. |

The name of a new session is `mcp-`, the name of the directory of the first pane, `-` and 8 random hexadecimal digits.
The directory of the first pane is `DIR` of `--cwd`, else the working directory.
Each character that a session name cannot have becomes `_`.
`list_panes` gives the name, and the server writes it on standard error.

| Problem | Message | Exit status |
| --- | --- | --- |
| An unknown option, or an option without its value | The usage | 2 |
| `--session` with a name that a session cannot have | `--session takes the name of a session: ...` and the rule | 2 |
| `--adopt` without `--session` | `--adopt needs --session NAME: the session to adopt` | 2 |
| `--cwd DIR` that is not a directory | `no directory DIR` | 1 |
| No `--cwd`, and the working directory was removed | `the current directory was removed, so give an absolute directory with --cwd` | 1 |
| `--session NAME` of a session that is not an agent session, without `--adopt` | `session NAME is not an agent session: give --adopt to make it one, which the server then never ends` | 1 |
| `--adopt` with a name that no session has | `no session is named NAME, so there is none to adopt` | 1 |
| `Control.enabled` is off, and the server makes a new session | `Control.enabled is off in FILE, and the tools need the control calls of the session: turn it on` | 1 |
| `Control.enabled` is off for the session of `--session NAME` | `Control.enabled is off for session NAME, and the tools need its control calls` | 1 |
| The mux daemon does not start | Why it did not start | 1 |

The server exits with status 0 when the client closes the standard input or the standard output of the server, and at SIGTERM or SIGHUP.
A failed read or write of the protocol gives status 1.
SIGINT and SIGKILL end the server at once.
At each end of the server, the daemon ends the session when the server bound it.

## The session

The server uses the mux daemon of this computer.
When no daemon runs, the server starts one.
The session is an [agent session](MUX.md#agent-sessions): it keeps its read access.
A window that shows it marks it with one badge at the start of its tab bar.
A window of the session detaches when it closes, also at the close of its last tab or pane, whatever `Mux.onClose` says.

| Case | End of the session |
| --- | --- |
| A session that the server created, or that it named with `--session` | When the server exits, also when it crashes or is killed. The mux daemon ends it. |
| The same, with `Mcp.endSessionOnExit` off | Never with the server. Start a new server with `--session NAME` to use it again. |
| Two servers with the same `--session` | When the last of the two exits |
| A session that the server adopted with `--adopt` | Never with the server. It is your session. |
| A session that a server adopted before, named by a later server with `--session` | Never with the server. It stays your session. |
| Any session | When its last pane closes, or with `afterglow mux kill NAME` |

Two servers on one session share its panes.
Each server knows only the commands that it ran itself.
Two servers that start at the same time with the same `--session NAME` share one session.
When the session of `--session NAME` ends while a new server joins it, the new server makes it again.

When the session ends while the server runs, each tool fails with a message that says so.
Restart the server to get a new session.

The mux daemon closes the connection of a server that does not read its answers (see [limits](MUX.md#limits)).
Then the server cannot use its session any more.
When the session ends with the server, the server binds it again on a new connection, and closes that connection.
Then the session ends, unless another server binds it.
The server binds again only through the same daemon process.
A daemon that started later can have a different session with the same name, and the server does not end it.
When the link closed because the daemon stopped, the session ended with the daemon.
Then each tool fails with a message that says so.

## Tools

Each tool that acts on a pane takes `pane`, the pane ID of the daemon.
`list_panes` and `open_pane` give the IDs.
A tool never acts on the pane that has the focus: a click in a window that shows the session does not move the agent.
Each result has a short text and the same data as structured content.

| Tool | Arguments | Result |
| --- | --- | --- |
| `list_panes` | None | `session`, and for each pane `id`, `tab`, `title`, `cwd`, `command`, `cols`, `rows`, `shell`, `input` and `last_exit` |
| `open_pane` | `where` (`tab` or `split`), `pane`, `direction`, `cwd`, `argv`, `env` | `pane`, the ID of the new pane |
| `run` | `pane`, `command`, `timeout_ms`, `tail_lines` | `status`, `exit_code`, `exit_codes`, `duration_ms`, `output`, `truncated`, `notes` |
| `wait` | `pane`, `until` (`finished`, `text`, `quiet` or `closed`), `pattern`, `quiet_ms`, `timeout_ms`, `tail_lines` | As `run`, and `matched` and `match` |
| `send` | `pane`, `text`, `paste`, `keys`, `signal` | None |
| `read` | `pane`, `scope` (`screen`, `scrollback` or `last_output`), `tail_lines`, `max_bytes` | `text`, `truncated` |
| `screenshot` | `pane`, `scope` (`pane`, `tab` or `window`) | A PNG image, and `width`, `height` and `scaled` |
| `close_pane` | `pane`, `end_session` | None |
| `open_project` | `name` | `name`, `panes` and `warnings` |
| `save_project` | `name`, `overwrite` | `name`, `path`, `panes` and `warnings` |
| `list_script_commands` | None | `commands`: for each, `script`, `command`, `summary`, `usage` and `stdin` |
| `run_script_command` | `pane`, `script`, `command`, `args`, `stdin`, `timeout_ms` | `exit_code`, `stdout`, `stderr`, `truncated` |

The fields of a pane are those of the [pane object](CONTROL.md#pane-object), and `shell` has the [shell states](CONTROL.md#shell-states).

Each tool tells the client what it does with its hints.

| Tool | Read only | Destructive | Idempotent | Open world |
| --- | --- | --- | --- | --- |
| `list_panes` | Yes | No | Yes | No |
| `open_pane` | No | No | No | No |
| `run` | No | Yes | No | No |
| `wait` | Yes | No | No | No |
| `send` | No | Yes | No | No |
| `read` | Yes | No | Yes | No |
| `screenshot` | Yes | No | Yes | No |
| `close_pane` | No | Yes | No | No |
| `open_project` | No | No | No | No |
| `save_project` | No | Yes | Yes | No |
| `list_script_commands` | Yes | No | Yes | No |
| `run_script_command` | No | Yes | No | No |

### open_pane

`where` `tab` opens a new tab.
With `pane`, the tab comes after the tab of that pane.
Without `pane`, the tab comes after the tab that has the focus, and starts in the directory of the server: `--cwd`, else its working directory.
`where` `split` splits `pane` on the side of `direction`: `left`, `right` (the default), `up` or `down`.

The new pane runs `argv`, or your shell with shell integration.
The server prepares the program with the settings of your config file, as `afterglow ctl --session` does.
`env` sets variables for the program: a string sets a variable, and `false` removes it.
`cwd` is an absolute directory, `~`, or a directory that starts with `~/`.
Without `cwd`, a pane with `pane` starts where `Window.workingDirectory` says.
With `inherit`, the default, it starts in the directory of `pane`.

While no window shows the session, its panes have the size of a new window (`Window.initialColumns` and `Window.initialRows`).
A split halves its pane.
When the pane has no room for a split, the tool fails and tells the agent to open a tab.
Open tabs to get many panes with no window.
While a window shows the session, a split with no room opens as a tab, as in the window.

### run

`run` types a shell command in a pane, and waits for the shell's next prompt.
It needs [shell integration](CONTROL.md#shell-states) in the pane.

1. The server makes sure that the shell of the pane shows an empty prompt. In a pane that the server opened, it first waits for the first prompt of the shell.
2. It sends the command as a paste, with the Enter key in the same write. The daemon writes them only while the shell shows an empty prompt.
3. It waits until the shell shows a new prompt.
4. It returns the exit codes and the text since the command was sent.

`timeout_ms` counts from the call, the wait for the first prompt included.
A command of several lines needs bracketed paste in the shell, the default of bash 5.1, zsh 5.1, fish and later versions.
Without bracketed paste, the shell runs each line at its line end, and `run` can end after the first line.

`run` refuses a pane in these cases:

| State of the pane | Message |
| --- | --- |
| No shell integration (`shell` is `none`) | `pane N has no shell integration: use send and wait` |
| A command runs | `pane N runs a command: call wait with until finished, or send keys ctrl+c` |
| The shell waits for the rest of a command | `pane N waits for the rest of a command: send keys ctrl+c` |
| Someone typed text at the prompt | `pane N has text at its prompt that someone typed: ask the user, or send keys ctrl+u to clear it` |
| A `run`, or a `wait` with `until` `finished`, of this server waits on the pane | `pane N has a run or a wait until finished of this server already: wait for it` |
| A pane that the server opened shows no prompt before the timeout | `pane N showed no prompt in T ms: its shell has no shell integration, or has not started. Use send and wait.` |
| The shell left its empty prompt after the check, for example when someone typed | `the command did not go: ...`. The command is not sent. |

`output` is the text from the command line to the next prompt, as the pane shows it.
It keeps the last `tail_lines` lines (`Mcp.tailLines` by default) and the last `Mcp.outputLimit` bytes.
`truncated` is true when either limit cut the start of the text.

`exit_codes` holds one exit code for each command output since the command was sent. It depends on the shell:

| Shell | A command of two lines |
| --- | --- |
| Bash | Bash runs each line as a command of its own. `exit_codes` has two entries, one for each line. |
| Zsh, Fish | The shell runs the two lines as one command. `exit_codes` has one code. |

`exit_code` is the last code of `exit_codes`.
An empty command or a comment runs nothing: `exit_codes` is empty, and `exit_code` is `null`.
`run` ends at the first prompt after the last command output.
A shell shows its prompt again when it redraws it, for example after a paste of several lines.
Thus a command of several lines that runs nothing, such as lines of comments only, ends at the timeout.

| Event | Result |
| --- | --- |
| The new prompt shows | `status` `finished` |
| The timeout comes first (`timeout_ms`, else `Mcp.waitTimeout`) | `status` `running`, with the output so far. The command goes on. Call `wait` with `until` `finished` on the pane. |
| The command is incomplete, for example an open quote or a `\` at the end | A tool error. The shell waits for the rest. Send the keys `ctrl+c` to drop the command. |
| Fish keeps an incomplete command in its editor, and starts nothing | At the timeout, `status` `running` and a note in `notes` that says so. The server does not wait for the command any more. |
| The pane closes, or the session ends | A tool error that says which |
| `Control.enabled` goes off | A tool error. The tools stop until the server restarts. |
| A pane decodes an image, and cannot answer for a moment (`TRY_AGAIN`) | The server asks again a moment later, within the timeout. It never sends the command two times. |

When you type in the pane while `run` waits, your command becomes part of the run.
When the client cancels a `run` after the command went, the command runs, and the server stops the wait.
The pane can change its width or clear its screen after the server sent the command. Then `output` holds only the output of the last command, `truncated` is true, and `notes` says so.
When the daemon cannot give the output, the result has the exit codes, an empty `output`, and a note in `notes` that says why. An example is a text that is larger than one answer can hold.
`notes` holds these notes also in the structured content, for a client that shows only that. It is empty when there is nothing to tell.

### wait

| `until` | `wait` ends when |
| --- | --- |
| `finished` | The command of a `run` of this server ends. The result is as for `run`. Without a run of this server, at a prompt, it answers at once with the exit code and the output of the last command. While a command of another client runs, it waits for that command. |
| `text` | New text in the pane matches `pattern` |
| `quiet` | No text changes in the pane for `quiet_ms` milliseconds, and at least two times `Control.eventInterval` |
| `closed` | The pane closes. A pane that closed before the wait answers at once. A pane ID that the server never saw fails. |

`pattern` is a regular expression of PCRE2, with at most 1024 characters.
`^` and `$` match at the start and the end of each line.
Each check reads only the text since the last check, and the line in progress.
Thus a prompt with no line end after it, such as `(gdb) `, matches, with the space before the cursor.
After a `send` to the pane, the next `wait` with `until` `text` reads from where the cursor was before the input.
Thus the prompt that the input answers does not match, also when the program has not read the input yet.
A match across more lines than came in since the last check can fail: match within one line.
When more than 1 MiB of text comes between two checks, the check reads only its last 1 MiB.
At the timeout, one more check reads the text that came last.
On the alternate screen, or after a program cleared the screen or the pane changed its width, a check reads the whole screen.
Thus a match can come from text that was on the screen before the wait.

The events of a pane come at most once in each `Control.eventInterval`, and as late.
Thus `quiet` waits at least two times that interval. The server reads the interval from its config file.

| Result field | Content |
| --- | --- |
| `status` | `finished`, `matched`, `quiet` or `closed` when it happened. `running` when the timeout came first. |
| `matched` | True when it happened |
| `match` | For `text`: the text that matched, at most `Mcp.outputLimit` bytes. A cut removes its start. |
| `output` | The text since the wait began. For `finished` after a `run` of this server, since the run began. For a match of `text`, the text that the check that matched read, and after a `send` all the text since the input went. |
| `notes` | As for `run` |

A `run` and a `wait` that the client gave a progress token send progress notifications.
The progress is the number of rows since the start, and the message is the newest line, at most 200 characters.
They come when the text of the pane changes, at most once in each `Control.eventInterval`.

When the client cancels a `run` or a `wait`, the server stops the wait.
The program in the pane keeps running.

### send

`send` sends input to the program of a pane, for interactive programs such as gdb, REPLs and full-screen programs.
Give one or more of these arguments. They go in this order:

| Argument | Goes as |
| --- | --- |
| `text` | Typed bytes, as they are |
| `paste` | A paste: between the paste marks when the program asked for them (bracketed paste), with each line end as Enter |
| `keys` | A list of [key names](CONTROL.md#key-names), such as `enter`, `ctrl+c` or `up` |
| `signal` | `INT`, `TERM` or `KILL`, to the program in the foreground of the pane |

Use `run` for shell commands.
After `send`, call `wait` or `read`.

When the pane decodes an image (`TRY_AGAIN`), the server sends that part again a moment later.
When a part fails after the parts before it went, the error names the parts that went: send only what did not go.

### read

| `scope` | Text |
| --- | --- |
| `screen` (the default) | The screen |
| `scrollback` | The scrollback and the screen |
| `last_output` | The output of the last command (shell integration) |

The text keeps its end: a cut removes its start.
The daemon also keeps the end when the text is larger than its 1 MiB.
`max_bytes` is at most `Mcp.outputLimit`.

### screenshot

`screenshot` needs a window that shows the session.
Without one, it fails, and tells the agent to ask you to run `afterglow mux attach NAME`.
The image is as the window draws it, with the shader packs.
An image with a side longer than `Mcp.screenshotMaxSize` is scaled down. `scaled` is then true.

### close_pane

`close_pane` closes a pane, and its program ends.
The last pane of the session needs `end_session` true: the session ends with it.
The mux daemon decides in the same step as the close whether the pane is the last one.
Thus two closes at the same time never end the session without `end_session` true.

### open_project

`open_project` opens a saved project in the session of the server.
It reads the project from `projects/` beside the config file that the server read, as `afterglow ctl open-project` does (see [project files](PROJECTS.md#project-files)).
The project opens into this session also when it has `"session": true`.
It opens no window, and it changes no other session and no window of yours.

The tabs of the project come after the tabs of the session.
Each pane runs the command of the project in its directory, or your shell, as a window starts it.
The panes have the size of the session's panes.
Each split gives its new pane half of the room of the pane that it splits: the ratios of the project do not apply.
The titles of the tabs and panes of the project do not apply either.
The focus goes to the active pane of the active tab of the project.

`panes` maps the ID of each pane in the project file to its pane ID in the session.
`warnings` says what the open had to change, for example a command that is not installed, which starts your shell instead.

| Problem | Result |
| --- | --- |
| No saved project has the name | A tool error that gives the names of the saved projects |
| The project file breaks a [limit](PROJECTS.md#project-files) | A tool error that names the limit |
| A pane has no room for its split while no window shows the session | A tool error that tells the agent to ask you to attach a window. The panes that the open made close. |
| The daemon refuses a pane | A tool error. The panes that the open made close. |
| The open fails, and its panes are the only panes of the session | The last of them stays open, so that the session does not end. The tool error says which pane stays. |
| The program of a pane ended before the next pane splits it | The next pane opens in a tab of its own, with a warning |
| The client cancels the call | The panes that opened stay, and no more open |

### save_project

`save_project` saves the tabs, splits, ratios, focus and directories of the session as a project.
It writes `projects/NAME.json` beside the config file that the server read, as `afterglow ctl save-project` does.
It needs no window: the mux daemon keeps the layout of the session.

| Field of a pane | Value |
| --- | --- |
| `cwd` | The directory that the shell reported, else the directory of its program, else your home directory with a warning |
| `command` | The command that started the pane: the `argv` of `open_pane`, or the command of a project. Never a command that was typed in a shell. A command over the limits of a project saves as the shell, with a warning. |
| `title` | The title that you gave the pane in a window |

The project has `"session": true`, as the save of a window of a mux session of this computer has.
Thus Switch Project opens it into a mux session.

A saved project of the name stays, and the tool fails, unless `overwrite` is true.
A project has at most 16 tabs and 32 panes. A save of a larger session fails, and names the limit.

### list_script_commands

`list_script_commands` lists the commands of the scripts that you enabled, as the window that shows the session has them.
`stdin` is true for a command that reads standard input.
When the window gives no list in `Mcp.waitTimeout`, the tool fails.

### run_script_command

`run_script_command` runs a command of a script, as `aglow run` runs it in `pane` (see [run a command](SCRIPTING.md#run-a-command)).
The command gets `args`, and `stdin` when the script marks it `stdin = true`.
`ev.source` is `shell`, and `ev.cwd` is the directory of the pane.
The tool waits until the command ends, and returns its exit status and its output.
`exit_code` is 0 to 255, as `aglow run` gives it: a script that gives another status gives 1.
`stdout` and `stderr` together are at most `Mcp.outputLimit` bytes, and `stderr` takes at most half of it.
A cut removes the start of the text, and `truncated` is then true.

The mux daemon runs no scripts. The window that shows the session runs the command, with its scripts and its settings (`Scripts.enabled` and the limits of `Scripts`).
`pane` must be a pane of the session.

| Problem | Result |
| --- | --- |
| No window shows the session | A tool error that tells the agent to ask you to run `afterglow mux attach NAME` |
| `Scripts.enabled` or `Control.enabled` is off in the window | A tool error that says so |
| No enabled script has the command | A tool error |
| `stdin` for a command that reads no standard input | A tool error |
| The timeout comes first (`timeout_ms`, else `Mcp.waitTimeout`) | A tool error. The server ends the run, as `aglow run` does at Ctrl+C. |
| The client cancels the call | The server ends the run |
| The window closes or detaches while the command runs | A tool error, and the run ends |

## Settings

The server reads the settings of the default config file once, when it starts.
The `--config` and `-o` options of a running Afterglow do not change them.
After a change, restart the server.
See the [configuration reference](CONFIGURATION.md#mcp).

| Setting | Default | Function |
| --- | --- | --- |
| `Mcp.waitTimeout` | 120000 ms | The timeout of `run`, `wait` and `run_script_command` when the call gives none, and of `list_script_commands`. A value above `Mcp.maxWaitTimeout` uses `Mcp.maxWaitTimeout`. |
| `Mcp.maxWaitTimeout` | 3600000 ms | The longest timeout that a call can ask for |
| `Mcp.outputLimit` | 65536 bytes | The most UTF-8 text that one result of `run`, `wait`, `read` or `run_script_command` carries |
| `Mcp.tailLines` | 200 | The lines of output of `run` and `wait` when the call gives no `tail_lines` |
| `Mcp.screenshotMaxSize` | 2048 px | The longest side of a screenshot |
| `Mcp.endSessionOnExit` | true | End the session with the server |

The session follows `Control.enabled` and `Control.eventInterval`, as each [session](MUX.md#control-of-a-session) does.
A change of `Control.enabled` reaches a session only through a window that shows the session.
To stop the agent of a session that no window shows, end the session with `afterglow mux kill NAME`.

## Limits

| Item | Limit | At the limit |
| --- | --- | --- |
| `command` of `run`, `text` and `paste` of `send` | One request to the daemon: 1 MiB, with the JSON escapes of the text | The tool fails, and sends nothing |
| A request line of the client | 13 MiB | The server answers with an error, and drops the line |
| Requests that wait for the mux daemon to read them | 64 MiB | The tool fails, and sends nothing |
| `keys` of `send` | 4096 | The tool fails |
| `pattern` of `wait` | 1024 characters | The tool fails |
| Panes of a session | 256 | `open_pane` and `open_project` fail |
| `args` of `run_script_command` | 256 arguments of 4096 UTF-16 units each, in one request of 1 MiB | The tool fails |
| `stdin` of `run_script_command` | `Scripts.commandStdinLimit` of the window | The tool fails, and the command does not run |
| `timeout_ms` | `Mcp.maxWaitTimeout` | The server uses the limit. A longer `quiet_ms` ends at the timeout. |

## Scope and safety

The tools act only on the session of the server, and only on the panes that the agent names.
Thus a text in a pane that tries to steer the agent cannot reach your other shells through these tools.
This is not a security boundary: an agent that runs commands can run `afterglow ctl` too.

The text of a pane is what its programs wrote. The agent must treat it as data, not as instructions.

The session reads its text and takes screenshots without `Control.allowReadText`: an agent session keeps its read access.

The tools cannot write, enable, load or install a script or a package.
A script runs later with your rights, so `run_script_command` runs only the commands of the scripts that you enabled.
A command can do all that its script can do.

A project runs its commands when it opens.
`save_project` writes the commands of the panes of the session, which the agent chose with `open_pane`, into a project file.
When you open a project that an agent saved, its commands run with your rights.
Examine the commands of the project file before you open it.
`save_project` replaces a saved project only with `overwrite` true.
