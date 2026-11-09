<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# The mux daemon

The mux daemon, `afterglow-mux`, keeps terminal sessions running while no window shows them.
A session is a set of panes with running programs. A session that `afterglow mux new` creates has one pane.
The `afterglow mux` commands create, list and end sessions. They also show a session in a window, and close its pane again.

The mux is opt-in. A normal launch of Afterglow does not use it.
A mux session and a normal `afterglow` launch can run at the same time.

## The daemon

`afterglow-mux` is one process with no Qt. It needs no display and no GPU.
It owns the sessions, the PTYs of their panes and the terminal state of each pane.
It reads no configuration file. `afterglow mux new` sends the values that a session needs (see [environment](#environment)).

`afterglow mux` runs the `afterglow-mux` in the directory of the `afterglow` executable. It does not search `PATH`.

### Start

`afterglow mux new` starts the daemon when no daemon answers on a [socket](#sockets).
`afterglow mux attach` without `NAME` starts it too when no session runs, because it then creates a session.
`afterglow mux attach NAME`, `afterglow mux detach`, `afterglow mux ls` and `afterglow mux kill` do not start the daemon.

The command uses the systemd user manager when all of these conditions are true:

- `XDG_RUNTIME_DIR` is `/run/user/UID`.
- The systemd user manager runs.
- Lingering is on for your user, or `XDG_SESSION_TYPE` is `wayland` or `x11`.

| Condition | How the daemon starts |
| --- | --- |
| All of the conditions are true | In its own systemd user unit, `afterglow-mux.service`, with `systemd-run --user` |
| One of the conditions is false | In a new session with `setsid`, outside the process group and the terminal of the command |
| `systemd-run` fails, and `afterglow-mux.service` does not exist | In a new session with `setsid` |
| `systemd-run` fails, and `afterglow-mux.service` exists | No second daemon starts. The command waits for the daemon of that unit, which starts or ends. |

`UID` is your user ID. Lingering is on when `/var/lib/systemd/linger/USER` exists (`loginctl enable-linger`).
The command finds the user manager through its socket, `$XDG_RUNTIME_DIR/systemd/private`.

`systemd-run` gives the daemon the `XDG_STATE_HOME` and `HOME` of the command, so that the daemon and its clients agree on the second socket.

The unit of the daemon is not the unit of a window.
When systemd-oomd stops the unit of a window, the sessions stay.
Without lingering, the user manager stops its units at your last logout.
For this reason, an SSH login without lingering starts the daemon with `setsid`.
A daemon that starts with `setsid` stays after your last logout, unless logind sets `KillUserProcesses=yes`.

`afterglow mux new` waits up to 5 seconds for a socket of the daemon to answer.
When the daemon does not start, the command stops with exit status 1.
It writes `afterglow mux new: ` and a message on standard error.
When `afterglow mux attach` creates the session, the message starts with `afterglow mux attach: `.

| Start | Result | Message |
| --- | --- | --- |
| `setsid` | The daemon reports an error before it exits, for example when it can use none of its sockets (see [sockets](#sockets)) | The error of the daemon, such as `no socket to listen on: ` and the reason for each socket. The command stops at once. |
| `setsid` | The daemon finds a socket that another daemon holds, and that daemon does not answer in 5 seconds | `afterglow-mux: another afterglow-mux listens on PATH, which does not answer` |
| `setsid` | The daemon exits before it listens and reports no error | `afterglow-mux ended before it listened`. The command stops at once, without the wait of 5 seconds. |
| `setsid` | No socket of the daemon answers in 5 seconds | `afterglow-mux did not start: no daemon answers on PATH`, with the path of each socket |
| systemd user unit | The unit is in the state `deactivating`, and no socket answers in 5 seconds | `afterglow-mux did not start: the daemon of afterglow-mux.service is ending. Try again when it has ended` |
| systemd user unit | No socket of the daemon answers in 5 seconds. Or the unit that the command started ends before its daemon answers, and no other daemon holds the lock of a socket. | `afterglow-mux did not start: see journalctl --user -u afterglow-mux`. The journal shows the error of the daemon. |
| systemd user unit | Another daemon holds the lock of a socket and does not answer in 5 seconds. A lock file that a killed daemon left holds no lock. | `afterglow-mux did not start: another afterglow-mux holds a socket and does not answer` |

Two commands can start a daemon at the same time. Then the second daemon finds the socket of the first daemon and exits.
Its command waits for the first daemon, because the first daemon can take some time before it answers.
When the unit `afterglow-mux.service` is still loaded because its daemon ends, `afterglow mux new` waits for the end of that unit.
When the unit is gone, the command starts the unit again one time, in the same 5 seconds.

### End

The daemon exits when it has no session and no open connection.
In the first 10 seconds after it starts, it waits for its first session and does not exit.
`SIGTERM`, `SIGINT` and `SIGHUP` stop the daemon. Its sessions and their programs end with it.
The daemon then ends with the same signal, so its parent sees why it ended.
A signal that the daemon gets as ignored stays ignored. Thus `nohup afterglow mux server` keeps running when its terminal closes.

## Commands

| Command | Function |
| --- | --- |
| `afterglow mux new` | Creates a session and shows it in a window. Options: `--name NAME`, `--cwd DIR`, `--detach`, `-- COMMAND...`. |
| `afterglow mux attach [NAME]` | Shows a session in a window. Option: `--no-takeover`. |
| `afterglow mux detach [NAME]` | Closes the pane of the session in its window. The session keeps running. |
| `afterglow mux ls` | Lists the sessions, with the columns `NAME`, `STATE` and `PANES` |
| `afterglow mux kill NAME` | Ends a session and its programs. Option: `--force`. |
| `afterglow mux server` | Runs `afterglow-mux` in the foreground |
| `afterglow mux --help` | Prints the usage |

`afterglow mux new` shows the new session in a window. With `--detach`, it creates the session without a window and prints its name.
The session starts in `DIR`, or in the current directory without `--cwd`. A directory that does not exist stops the command.
Without `COMMAND`, the session runs the program of a new pane in a window: `Terminal.shell`, else your login shell.
The pane has the size of a new window: `Window.initialColumns` by `Window.initialRows` cells.
Without `--name`, the daemon gives the session a number as its name, from `1`.
The daemon does not give the same number two times while it runs. A number that a session name has is skipped.
A name that a session has already is refused.

`afterglow mux ls` shows `attached` in the `STATE` column when a window shows the session, else `detached`.
The `PANES` column shows the number of panes of the session.
Without a daemon, `afterglow mux ls` and `afterglow mux kill` report that there are no sessions.

When `Terminal.confirmClose` is on and a program runs in the session, `afterglow mux kill` asks first.
It asks only when its standard input is a terminal.
Without a terminal, it refuses and names `--force`.
The command itself is not a running program when it runs in the foreground of a pane of that session.
`--force` ends the session without a question.

### Windows of a session

`afterglow mux attach NAME` opens a window that shows session `NAME`.
The window opens in the Afterglow that runs on your display, as the window of any launch.
When a window of that Afterglow shows the session already, that window comes to the front, and no new window opens.
Without `NAME`, the command shows the only session. When no session runs, it creates one, as `afterglow mux new` does.
When more than one session runs, it stops and names them.

When no Afterglow runs on your display, the command becomes that Afterglow.
It then stays in the foreground of its terminal until you close its last window, and its exit status is the status of Afterglow.
Later launches on the display open their windows in it.

One window at a time shows a session.
When another window shows the session, its pane of the session closes, and the session moves to the new window.
A window that has no other pane closes with it.
With `--no-takeover`, the command refuses instead, with exit status 1.
A window of the same Afterglow does not count: it comes to the front, also with `--no-takeover`.

`afterglow mux attach` and `afterglow mux new` without `--detach` need a Wayland display.
The display is `WAYLAND_DISPLAY`, by default `wayland-0`, as a socket in `XDG_RUNTIME_DIR` unless it is a path.
When that socket does not exist, or `QT_QPA_PLATFORM` names a platform that is not Wayland, they stop with exit status 1 and create no session.
When `afterglow mux new` cannot run `afterglow` for the window, it ends the session that it created.
When the window fails after it started, the session keeps running. `afterglow mux attach` shows it, and `afterglow mux kill` ends it.

The window shows the screen and the scrollback of the session, and sends what you type to its programs.
The window measures the pane with its own font, and the daemon resizes the pane to that size.
The session keeps the terminal settings and the colors of the `afterglow mux new` that created it.
The Reset and Clear actions of the window reset or clear the terminal of the session.

| Event | Result |
| --- | --- |
| You close the window, the tab or the pane of the session | `Mux.onClose`: `kill` (the default) ends the session and its programs, as in a window outside a session. `detach` keeps the session running. |
| You quit Afterglow | The session keeps running, whatever `Mux.onClose` says. At your last logout, the daemon can end too (see [start](#start)). |
| `afterglow mux detach NAME` | The pane of the session closes. The session keeps running. In a pane of a session, `NAME` defaults to that session. |
| Another window shows the session (`afterglow mux attach`) | The pane of the session closes, with a notice. The session keeps running. |
| The window crashes, or its connection to the daemon breaks | The session keeps running. A window that still runs closes the pane, with a notice. |
| The program of the pane exits | The pane shows the last output of the program, then closes as a pane does in a window outside a session. The session ends. |
| `afterglow mux kill NAME` | The session ends, and its pane closes |

In each case, a window that has no other pane closes with the pane of the session.

A new tab or a split in the window of a session runs a program of the window, not of the session.
These programs end with the window.
A project saves the pane of a session as a shell in the directory of that pane. A project does not keep sessions.

The window of a session does not show these yet:

- Bells, desktop notifications and the clipboard of the programs (see [what the daemon answers](#what-the-daemon-answers))
- Images, and extra cursors
- Command marks of shell integration (OSC 133): the last output, the jumps between prompts, a click on the prompt, and Notify on Next Command Finish
- The status of programs (OSC 7501), the shell contexts in the title, and user variables
- Visibility reports (mode 2033): the program always hears that the window is visible
- The directory of the program when its shell does not report it (OSC 7). New tabs and splits then start in your home directory.
- A selection or a search match in a region that a program scrolls, as an editor does: it stays on its rows, not on its text

These commands and options are not available. Each stops with exit status 1 and writes this message on standard error:

| Command | Message |
| --- | --- |
| `--ssh` of `afterglow mux new`, `attach`, `detach`, `ls` and `kill` | `afterglow mux COMMAND: sessions on other hosts are not available yet` |

### Exit status

| Status | Meaning |
| --- | --- |
| 0 | The command did its work. `afterglow mux ls` without sessions also exits with 0. |
| 1 | The command failed and wrote why on standard error. Examples: no session has the name, the daemon did not start, a command is not available. `afterglow mux kill` also exits with 1 when you do not answer `y`, `Y` or `yes` to its question. |
| 2 | A usage error, for example an unknown command, option or argument, or an option without its value. The command also writes the usage on standard error. |
| 2 | A session name that is not valid, for `afterglow mux new --name`, `attach` or `detach`. The command writes only the rule for names. |
| 126 | `afterglow mux server` found `afterglow-mux` but cannot run it. `afterglow mux new` or `attach` found `afterglow` but cannot run it. |
| 127 | `afterglow mux server` cannot find `afterglow-mux` next to `afterglow`. `afterglow mux new` or `attach` cannot find `afterglow`. |

The usage errors include these:

- No command, or an unknown command
- An argument to `ls` or `server`
- `afterglow mux kill` without a session name
- `afterglow mux detach` without a session name, outside a session pane

A window that `afterglow mux attach` opens in an Afterglow that runs already reports its errors with the prefix `afterglow: `.

## Sockets

The daemon listens on each socket of this table that it can use.
A client tries the sockets in the order of the table and uses the first one that answers.

| Order | Directory | Socket in the directory |
| --- | --- | --- |
| 1 | `$XDG_RUNTIME_DIR/afterglow-mux` | `socket` |
| 2 | `$XDG_STATE_HOME/afterglow-mux`, by default `~/.local/state/afterglow-mux` | `ID.sock` |

`ID` comes from the machine ID of the computer, in `/etc/machine-id`.
It is 32 hexadecimal digits of a keyed hash of the machine ID, so the file name does not show the machine ID.
Thus computers that share a home directory over NFS have different sockets, and a new host name does not move the socket.
When `/etc/machine-id` cannot be read, or does not hold 32 hexadecimal digits, `ID` is the host name. Each `/` in it becomes `_`.
Without a host name, `ID` is `localhost`.
A relative path in `XDG_RUNTIME_DIR` or `XDG_STATE_HOME` counts as not set.
Without `XDG_RUNTIME_DIR`, the daemon has only the second socket.
When neither `XDG_STATE_HOME` nor `HOME` is an absolute path, the daemon has only the first socket.
The second socket stays when the system removes `/run/user/UID` at your last logout.
The sockets do not depend on the display.

The daemon creates each directory with mode 0700.
It uses a directory only when the directory belongs to you and has mode 0700.
The daemon refuses a connection from a process of another user (`SO_PEERCRED`).

Next to each socket, the daemon holds a lock on the file `SOCKET.lock`.
Another daemon can hold the lock of one of the sockets.
Then the new daemon exits with status 1 and the message `another afterglow-mux listens on PATH`.
Your sessions stay in the daemon that holds the lock.
`afterglow mux new` then waits for the daemon that holds the lock (see [start](#start)).
A socket can fail for another reason, such as a directory with the wrong mode.
Then the daemon writes the reason on standard error and uses the other socket.
When the daemon has no socket, it exits with status 1.
When the daemon exits, it removes its sockets and its lock files.

For each window that shows a session, the daemon makes one more socket, `attach-` and 16 hexadecimal digits, then `.sock`.
It is in the directory of the first socket that the daemon still listens on.
Only the process that asked for it can connect, and the daemon removes it when that process connects, or after 5 seconds.
A daemon that starts removes the attach sockets that a daemon before it left in `$XDG_RUNTIME_DIR/afterglow-mux`.
It leaves the state directory as it is, because other computers can share it.
Thus an attach socket that a killed daemon left there stays until you remove it.

## Environment

The programs of a session run with the environment of `afterglow mux new`, not with the environment of the daemon.
Thus a session does not get variables of the daemon's systemd unit, such as `INVOCATION_ID`.
`afterglow mux new` prepares the program as a window prepares a new pane.
It adds shell integration, `TERM`, `COLORTERM`, `TERM_PROGRAM` and the values of `Terminal.env`.
The programs of the session get the umask of `afterglow mux new`.
They get the soft limit of open files that the daemon had before it raised its own (see [limits](#limits)).

A new pane leaves out the variables that name another terminal or the shell instance that started the command.
A window, an overlay and a session do this the same way.
A variable that `Terminal.env` sets stays.

| Variables | Reason |
| --- | --- |
| `TMUX`, `TMUX_PANE`, `STY`, `WINDOWID`, `VTE_VERSION` | They name tmux, screen, an X11 window or a VTE terminal |
| `KITTY_WINDOW_ID`, `KITTY_PID`, `KITTY_LISTEN_ON`, `KITTY_PUBLIC_KEY` | They name a Kitty window |
| `WEZTERM_PANE`, `WEZTERM_UNIX_SOCKET` | They name a WezTerm pane |
| `KONSOLE_DBUS_SERVICE`, `KONSOLE_DBUS_SESSION`, `KONSOLE_DBUS_WINDOW`, `KONSOLE_VERSION` | They name a Konsole session |
| `ITERM_SESSION_ID`, `WT_SESSION` | They name an iTerm2 or Windows Terminal session |
| `XDG_ACTIVATION_TOKEN`, `DESKTOP_STARTUP_ID` | They belong to the launch of the command |
| `AFTERGLOW_RESULT`, `OLDPWD`, `SHLVL` | They belong to the shell that ran the command |

`PWD` stays. `SSH_TTY`, `SSH_CONNECTION`, `SSH_CLIENT` and `GPG_TTY` stay too, because they describe the login, not a terminal.

The daemon sets these variables in each pane of a session:

| Variable | Value |
| --- | --- |
| `AFTERGLOW_SOCKET` | The first socket on which the daemon listens and that still exists when the session starts (see [sockets](#sockets)) |
| `AFTERGLOW_PANE_ID` | The pane ID in the daemon |
| `AFTERGLOW_SESSION` | The session name |

The daemon removes `AFTERGLOW_OVERLAY`.
`Terminal.env` cannot set `AFTERGLOW_SESSION` or `AFTERGLOW_OVERLAY`.
An `afterglow` that starts from a session pane does not give `AFTERGLOW_SESSION` to its own panes.

`afterglow mux new` also sends the terminal settings of your configuration, such as `Terminal.scrollback` and the theme colors.
The session keeps these values. A later change of the configuration does not change a running session.
`afterglow mux new` reads no colors from the palette of the desktop.
When `Colors.theme` names a dark and a light theme, the session gets the dark theme.
With a `plasma` theme, the session gets only the colors that `kdeglobals` sets, over the theme.
When `kdeglobals` sets no background, `plasma` alone takes `afterglow-dark`.

### Control in a session pane

A window that shows a session has a pane for it. The control socket of that window lists this pane, and acts on it as on its other panes.
Text that `afterglow ctl send-text` sends to it goes to the program of the session.
A session that no window shows has no pane in a window, and no control socket of a window lists it.
A pane ID in the daemon and a pane ID in a window can have the same value.

In a session pane, `afterglow ctl`, `aglow run` and `afterglow ssh` connect to the daemon through `AFTERGLOW_SOCKET`.
The daemon answers each call and each subscription to the `control` and `commands` targets with the error `CALL_FAILED`, also while a window shows the session.
The message is `a session pane takes no control calls: the mux daemon answers only mux calls`.
Thus `afterglow ctl`, `afterglow ctl watch` and `aglow run` fail with that message.
`afterglow ssh` connects without [remote commands](CONTROL.md#remote-commands). With `-v`, it writes that message as the reason.

## What the daemon answers

The daemon reads the PTY output and parses it into the terminal of the pane, whether or not a window shows the session.
While no window shows it, the session keeps running, scrollback grows to the `Terminal.scrollback` limit, and the programs do not pause.
The window that shows a session does not change these answers yet.

| Request of a program | Result |
| --- | --- |
| Queries that the terminal answers itself (DA1, DSR, color queries) | The daemon answers |
| Title and working directory | The daemon keeps the last value |
| Notification query (OSC 99 `p=?`) | The daemon gives the answer of a window |
| Query of the notifications on screen (OSC 99 `p=alive`) | The daemon answers with an empty list, because no notification is on screen |
| Query of the socket of an ssh session (OSC 24569) | The daemon answers that there is none (`error=off`) |
| Bell, notification, clipboard write | The daemon drops it. A clipboard write that asks for a status (OSC 5522) gets `EPERM`. |
| Clipboard read (OSC 52 and OSC 5522) | Refused, as when you deny the request |
| File transfer (OSC 5113) | Refused with `EPERM`, as when you deny the request. A transfer with `q=2` asks for no status and gets no answer. |
| Drag and drop (OSC 72) | A program that asks which types the terminal supports gets the answer of a window. A drag offer (`t=o`) gets `EPERM`. No drop arrives, and no drag starts. |
| File download (OSC 1337 `File`) | Dropped |
| Images | RGB, RGBA and sixel images work in the daemon. PNG (kitty graphics `f=100`) and iTerm2 images are refused, because the daemon decodes no compressed images. A window does not show images yet. |

## Limits

| Item | Limit | At the limit |
| --- | --- | --- |
| Session name | 1 to 64 characters: ASCII letters, digits, `_` and `-`. The first character is an ASCII letter, a digit or `_`. | `afterglow mux new` refuses the name with exit status 2 |
| Socket path | 107 bytes | The daemon and the client leave out a socket with a longer path. When no socket is left, they stop and name the path. |
| Open files | The hard limit of `RLIMIT_NOFILE`. The daemon raises its soft limit to the hard limit when it starts. | A new connection closes at once. A new session gets an error that says why its program cannot start. |
| Request line | 1 MiB, with its newline | The daemon answers `MALFORMED_REQUEST` and closes the connection |
| Lines that do not parse, one after the other | 16 | Each line gets `MALFORMED_REQUEST`. The daemon closes the connection after its answer to the 17th line. |
| Waiting output to one client | 32 MiB | The daemon closes the connection to a client that does not read its answers |
| Environment, arguments and directory of a session | UTF-8 text | `afterglow mux new` changes each byte that is not UTF-8 to U+FFFD |
| Wait for a window to connect to its attach socket | 5 seconds | The daemon removes the socket, and `afterglow mux attach` stops with exit status 1 |
| Input of a window that the program does not read | 32 MiB in the queue of the PTY, 4 MiB in the daemon, then 32 MiB in the window | The window refuses more input and shows a notice, as for a pane of its own. Closing the window with `Mux.onClose` kill still ends the session. |
| Frames to a window | About 2 MiB of state in each frame, and at most 4 MiB, the largest frame of the stream | Larger state goes in more frames, so a slow window gets fewer frames. It never holds a queue of old ones. |

The daemon sets no limit on the number of sessions or connections. Each session and each connection uses open files.
