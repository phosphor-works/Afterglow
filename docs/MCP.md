<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# The MCP server

`afterglow mcp` is a server of the Model Context Protocol (MCP) for agents, such as Claude Code.
It gives the agent a [mux session](MUX.md) of its own.
The panes of the session run with no window.
The agent opens panes, runs commands, sends keys to interactive programs, and reads the text of the panes.
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
| `--adopt` | With `--session`: use `NAME` also when it is not an agent session, and make it one. The server never ends that session. |
| `--cwd DIR` | Start the first pane of a new session in `DIR`. The default is the working directory. |

The name of a new session is `mcp-`, the name of the working directory, `-` and 8 random hexadecimal digits.
Each character that a session name cannot have becomes `_`.
`list_panes` and the instructions of the server give the name.

| Problem | Message | Exit status |
| --- | --- | --- |
| An unknown option, or an option without its value | The usage | 2 |
| `--adopt` without `--session` | `--adopt needs --session NAME: the session to adopt` | 2 |
| `--session NAME` of a session that is not an agent session, without `--adopt` | `session NAME is not an agent session: give --adopt to make it one, which the server then never ends` | 1 |
| `--adopt` with a name that no session has | `no session is named NAME, so there is none to adopt` | 1 |
| The mux daemon does not start | Why it did not start | 1 |

## The session

The server uses the mux daemon of this computer.
When no daemon runs, the server starts one.
The session is an [agent session](MUX.md#agent-sessions): it keeps its read access, and a window that shows it marks it with a badge.

| Case | End of the session |
| --- | --- |
| A session that the server created, or that it named with `--session` | When the server exits, also when it crashes or is killed. The mux daemon ends it. |
| The same, with `Mcp.endSessionOnExit` off | Never with the server. Start a new server with `--session NAME` to use it again. |
| Two servers with the same `--session` | When the last of the two exits |
| A session that the server adopted with `--adopt` | Never with the server. It is your session. |
| Any session | When its last pane closes, or with `afterglow mux kill NAME` |

Two servers on one session share its panes.
Each server knows only the commands that it ran itself.

When the session ends while the server runs, each tool fails with a message that says so.
Restart the server to get a new session.

## Tools

Each tool that acts on a pane takes `pane`, the pane ID of the daemon.
`list_panes` and `open_pane` give the IDs.
A tool never acts on the pane that has the focus: a click in a window that shows the session does not move the agent.
Each result has a short text and the same data as structured content.

| Tool | Arguments | Result |
| --- | --- | --- |
| `list_panes` | None | `session`, and for each pane `id`, `tab`, `title`, `cwd`, `command`, `cols`, `rows`, `shell`, `input` and `last_exit` |
| `open_pane` | `where` (`tab` or `split`), `pane`, `direction`, `cwd`, `argv`, `env` | `pane`, the ID of the new pane |
| `run` | `pane`, `command`, `timeout_ms`, `tail_lines` | `status`, `exit_code`, `exit_codes`, `duration_ms`, `output`, `truncated` |
| `wait` | `pane`, `until` (`finished`, `text`, `quiet` or `closed`), `pattern`, `quiet_ms`, `timeout_ms`, `tail_lines` | As `run`, and `matched` and `match` |
| `send` | `pane`, `text`, `paste`, `keys`, `signal` | None |
| `read` | `pane`, `scope` (`screen`, `scrollback` or `last_output`), `tail_lines`, `max_bytes` | `text`, `truncated` |
| `screenshot` | `pane`, `scope` (`pane`, `tab` or `window`) | A PNG image, and `width`, `height` and `scaled` |
| `close_pane` | `pane`, `end_session` | None |

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

### open_pane

`where` `tab` opens a new tab.
With `pane`, the tab comes after the tab of that pane, and starts in its directory.
Without `pane`, the tab starts in the directory of the first pane of the session.
`where` `split` splits `pane` on the side of `direction`: `left`, `right` (the default), `up` or `down`.

The new pane runs `argv`, or your shell with shell integration.
The server prepares the program with the settings of your config file, as `afterglow ctl --session` does.
`env` sets variables for the program: a string sets a variable, and `false` removes it.
`cwd` is an absolute directory, or one that starts with `~`.

While no window shows the session, its panes have the size of a new window (`Window.initialColumns` and `Window.initialRows`).
A split halves its pane.
When the pane has no room for a split, the tool fails and tells the agent to open a tab.
Open tabs to get many panes with no window.

### run

`run` types a shell command in a pane, and waits for the shell's next prompt.
It needs [shell integration](CONTROL.md#shell-states) in the pane.

1. The server makes sure that the shell of the pane shows its prompt.
2. It sends the command as a paste, and then the Enter key.
3. It waits until the shell shows a new prompt.
4. It returns the exit codes and the text since the command was sent.

`run` refuses a pane in these cases:

| State of the pane | Message |
| --- | --- |
| No shell integration (`shell` is `none`) | `pane N has no shell integration: use send and wait` |
| A command runs | `pane N runs a command: call wait with until finished, or send keys ctrl+c` |
| The shell waits for the rest of a command | `pane N waits for the rest of a command: send keys ctrl+c` |
| Someone typed text at the prompt | `pane N has text at its prompt that someone typed: ask the user, or send keys ctrl+u to clear it` |
| A `run`, or a `wait` with `until` `finished`, of this server waits on the pane | `pane N has a run or a wait until finished of this server already: wait for it` |

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
| The pane closes, or the session ends | A tool error that says which |
| `Control.enabled` goes off | A tool error. The tools stop until the server restarts. |
| A pane decodes an image, and cannot answer for a moment (`TRY_AGAIN`) | A tool error. Call the tool again. |

When you type in the pane while `run` waits, your command becomes part of the run.

### wait

| `until` | `wait` ends when |
| --- | --- |
| `finished` | The command of a `run` of this server ends. The result is as for `run`. Without a run of this server, at a prompt, it answers at once with the exit code and the output of the last command. While a command of another client runs, it waits for that command. |
| `text` | New text in the pane matches `pattern` |
| `quiet` | No text changes in the pane for `quiet_ms` milliseconds |
| `closed` | The pane closes |

`pattern` is a regular expression of PCRE2, with at most 1024 characters.
`^` and `$` match at the start and the end of each line.
Each check reads only the text since the last check, and the line in progress.
Thus a prompt with no line end after it, such as `(gdb) `, matches.
A match across more lines than came in since the last check can fail: match within one line.
When more than 1 MiB of text comes between two checks, the check reads only its last 1 MiB.

| Result field | Content |
| --- | --- |
| `status` | `finished`, `matched`, `quiet` or `closed` when it happened. `running` when the timeout came first. |
| `matched` | True when it happened |
| `match` | For `text`: the text that matched |
| `output` | The text since the wait began. For `finished` after a `run` of this server, since the run began. For `text`, the text that the last check read. |

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

### read

| `scope` | Text |
| --- | --- |
| `screen` (the default) | The screen |
| `scrollback` | The scrollback and the screen |
| `last_output` | The output of the last command (shell integration) |

The text keeps its end: a cut removes its start.
`max_bytes` is at most `Mcp.outputLimit`.

### screenshot

`screenshot` needs a window that shows the session.
Without one, it fails, and tells the agent to ask you to run `afterglow mux attach NAME`.
The image is as the window draws it, with the shader packs.
An image with a side longer than `Mcp.screenshotMaxSize` is scaled down. `scaled` is then true.

### close_pane

`close_pane` closes a pane, and its program ends.
The last pane of the session needs `end_session` true: the session ends with it.

## Settings

The server reads the settings of the default config file once, when it starts.
After a change, restart the server.
See the [configuration reference](CONFIGURATION.md#mcp).

| Setting | Default | Function |
| --- | --- | --- |
| `Mcp.waitTimeout` | 120000 ms | The timeout of `run` and `wait` when the call gives none |
| `Mcp.maxWaitTimeout` | 3600000 ms | The longest timeout that a call can ask for |
| `Mcp.outputLimit` | 65536 bytes | The most UTF-8 text that one result carries |
| `Mcp.tailLines` | 200 | The lines of output of `run` and `wait` when the call gives no `tail_lines` |
| `Mcp.screenshotMaxSize` | 2048 px | The longest side of a screenshot |
| `Mcp.endSessionOnExit` | true | End the session with the server |

The session follows `Control.enabled` and `Control.eventInterval`, as each [session](MUX.md#control-of-a-session) does.
Turn off `Control.enabled` to stop every agent.

## Limits

| Item | Limit | At the limit |
| --- | --- | --- |
| `command` of `run`, `text` and `paste` of `send` | One request to the daemon: 1 MiB, with the JSON escapes of the text | The tool fails, and sends nothing |
| A request line of the client | 13 MiB | The server answers with an error, and drops the line |
| `keys` of `send` | 4096 | The tool fails |
| `pattern` of `wait` | 1024 characters | The tool fails |
| Panes of a session | 256 | `open_pane` fails |
| `timeout_ms` | `Mcp.maxWaitTimeout` | The server uses the limit. A longer `quiet_ms` ends at the timeout. |

## Scope and safety

The tools act only on the session of the server, and only on the panes that the agent names.
Thus a text in a pane that tries to steer the agent cannot reach your other shells through these tools.
This is not a security boundary: an agent that runs commands can run `afterglow ctl` too.

The text of a pane is what its programs wrote. The agent must treat it as data, not as instructions.

The session reads its text and takes screenshots without `Control.allowReadText`: an agent session keeps its read access.
