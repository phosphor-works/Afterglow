<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# The mux daemon

The mux daemon, `afterglow-mux`, keeps terminal sessions running while no window shows them.
A session is a set of tabs and split panes with running programs. A session that `afterglow mux new` creates starts with one pane.
The `afterglow mux` commands create, list and end sessions. They also show a session in a window, and close that window again.

The mux is opt-in. A normal launch of Afterglow does not use it.
A mux session and a normal `afterglow` launch can run at the same time.

## The daemon

`afterglow-mux` is a daemon with no Qt. It needs no display and no GPU.
A second process of it starts a process for each image that a session decodes (see [what the daemon answers](#what-the-daemon-answers)).
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

A daemon of another build of Afterglow can speak another protocol.
Then `afterglow mux new`, `attach`, `detach`, `ls` and `kill` stop with exit status 1, and no window opens.
The message is `afterglow-mux speaks protocol N and this afterglow speaks M: end its sessions with the afterglow that started it, or wait until they end`.
`afterglow mux attach NAME` writes it after `afterglow: `, and the other commands after `afterglow mux COMMAND: `.

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
| `afterglow mux detach [NAME]` | Closes the window of the session. The session keeps running. |
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
The `PANES` column shows the number of panes in the session.
Without a daemon, `afterglow mux ls` and `afterglow mux kill` report that there are no sessions.

When `Terminal.confirmClose` is on and a program runs in the session, `afterglow mux kill` asks first.
It asks only when its standard input is a terminal.
Without a terminal, it refuses and names `--force`.
The command itself is not a running program when it runs in the foreground of a session pane of that session.
`--force` ends the session without a question.

The `--ssh` option of `afterglow mux new`, `attach`, `detach`, `ls` and `kill` is not available yet.
With it, the command stops with exit status 1 and writes `afterglow mux COMMAND: sessions on other hosts are not available yet` on standard error.

### Windows of a session

`afterglow mux attach NAME` opens a window that shows session `NAME`: its tabs and its split panes.
Each pane of the window is a pane of the session, a session pane.
The window opens in the Afterglow that runs on your display, as the window of any launch.
When a window of that Afterglow shows the session already, that window comes to the front with the tab that it shows.
No new window opens then.
Without `NAME`, the command shows the only session. When no session runs, it creates one, as `afterglow mux new` does.
When more than one session runs, it stops and names them.

When no Afterglow runs on your display, the command becomes that Afterglow.
It then stays in the foreground of its terminal until you close its last window, and its exit status is the status of Afterglow.
Later launches on the display open their windows in it.

One window at a time shows a session.
When another window shows the session, the first window closes, and the session moves to the new window.
With `--no-takeover`, the command refuses instead, with exit status 1.
A window of the same Afterglow does not count. That window comes to the front, also with `--no-takeover`.

`afterglow mux attach` and `afterglow mux new` without `--detach` need a Wayland display.
The display is `WAYLAND_DISPLAY`, by default `wayland-0`, as a socket in `XDG_RUNTIME_DIR` unless it is a path.
When that socket does not exist, or `QT_QPA_PLATFORM` names a platform that is not Wayland, they stop with exit status 1 and create no session.
When `afterglow mux new` cannot run `afterglow` for the window, it ends the session that it created.
When the window fails after it started, the session keeps running. `afterglow mux attach` shows it, and `afterglow mux kill` ends it.

The window shows the screen and the scrollback of each pane, and sends what you type to its programs.
The window measures each pane with its own font, and the daemon resizes the pane to that size.
The Reset and Clear actions of a pane reset or clear the terminal of that pane in the daemon.

| Event | Result |
| --- | --- |
| You close a pane or a tab, and the window has other panes | The programs of the pane or the tab end, as in a window outside a session. The session keeps running. |
| You close the window, or its last pane or tab | `Mux.onClose`: `kill` (the default) ends the session and its programs, as in a window outside a session. `detach` keeps the session running, with its tabs and splits. |
| You quit Afterglow | The session keeps running, whatever `Mux.onClose` says. When a window stops the quit with a question, `Mux.onClose` applies again to the windows that stay open. At your last logout, the daemon can end too (see [start](#start)). |
| `afterglow mux detach NAME` | The window closes. The session keeps running. In a session pane, `NAME` defaults to the session that holds that pane (see [detach in a session pane](#detach-in-a-session-pane)). |
| Another window shows the session (`afterglow mux attach`) | The window closes, with a notice. The session keeps running. |
| The window crashes, or its connection to the daemon breaks | The session keeps running. A window that still runs closes, with a notice. |
| The program of a pane exits | The pane shows the last output of the program, then closes as a pane does in a window outside a session. After the last program of the session, the session ends. |
| `afterglow mux kill NAME` | The session ends, and its window closes |

With `Mux.onClose` at `detach`, closing the window, or its last tab or pane, asks nothing about the programs of the session.
They keep running. `Terminal.confirmClose` still asks about the programs of overlays, which end.

### Detach in a session pane

In a session pane, `afterglow mux detach` without `NAME` asks the daemon which session holds the pane of `AFTERGLOW_PANE_ID`.
Thus a pane that moved to another session (see [move panes and tabs](#move-panes-and-tabs)) finds the session that holds it now.
The command runs in a session pane when `AFTERGLOW_SESSION` is set. Without it, `NAME` is necessary (see [exit status](#exit-status)).

| Problem | Message | Exit status |
| --- | --- | --- |
| `AFTERGLOW_PANE_ID` is not set, or is not a pane ID | `afterglow mux detach: AFTERGLOW_PANE_ID names no pane of the mux daemon` | 1 |
| No session of the daemon has the pane | `afterglow mux detach: no session has pane ID` | 1 |

### Tabs and splits

A new tab or a split in the window of a session starts a new pane of the session.
This is true for the keys, the menu, scripts and `afterglow ctl`.
The program of the new pane runs under the daemon, with the environment of the session (see [environment](#environment)).
The window prepares the program with its own settings, such as `Terminal.shell` and `Terminal.env`.
The terminal of the new pane gets the terminal settings of the session (see [settings of a session pane](#settings-of-a-session-pane)).
A session has at most 256 panes (see [limits](#limits)).

The daemon keeps the tabs and splits of the session.
The window sends them after each change. The next window that shows the session shows them again.

| Item | What the daemon keeps |
| --- | --- |
| Tabs | Their order, and the tab that shows |
| Splits | The direction and the ratio of each split |
| Focus | The pane that has the focus in each tab |
| Zoom | The zoomed pane of each tab |
| Titles | The titles that you gave to the tabs and to the panes |

While no window shows the session, a pane whose program exits leaves its tab, as in a window.

New Window (`new_window`) in the window of a session opens a window outside the session.
Its first pane starts in the directory that a new tab of the focused pane gets (see [working directory](#working-directory)).
Its programs end with that window, as in any window outside a session.

Save Project is not possible in the window of a session, because the daemon keeps its tabs and splits.
The window shows the notice `Session NAME keeps the tabs and splits of this window. A window of a mux session cannot be saved as a project.`
`afterglow ctl save-project` fails with the same message.

### Working directory

With `Window.workingDirectory` at `inherit`, the default, a new tab or split starts in the directory of the pane that it comes from.
The shell of that pane reports its directory with OSC 7, OSC 9;9, OSC 633 or iTerm2 `CurrentDir`.
When it reports no directory that exists on this computer, the window asks the daemon.
The daemon gives the directory of the program in the foreground.
When the daemon gives none, the new programs start in your home directory.

The `cwd` that scripts and `afterglow ctl` read is the directory that the shell reported, when it exists on this computer.
For it, the window does not ask the daemon, because scripts read every pane often.

### Move panes and tabs

A pane or a tab moves in the window of a session as in any window, to another tab or beside another pane.
A pane or a tab that you move to a new window leaves the session. These moves take a pane or a tab to a new window:

- A drag of a tab out of the tab bar
- **Detach Tab to New Window** in the tab menu, and the `detach_tab` action
- **Move Pane to New Window**, and the `move_pane_to_window` action
- `afterglow ctl move-pane 0 new-window` and `afterglow ctl detach-tab`
- The script effects `move_pane` with the placement `new-window`, and `detach_tab`

The programs of the pane or the tab go on in a new session, which gets the next number as its name.
A new window shows that session.
The new session has the environment and the terminal settings of the first session.

The pane gets a new pane ID in the window, and the tab gets a new tab ID.
Control clients get `paneClosed` and then `paneCreated`, not `paneMoved`.
Scripts get `on_pane_closed` and then `on_pane_created`. The `pane_id` of the result of `move_pane` and `detach_tab` is the new pane ID.
The pane ID in the daemon stays the same, and so does the `AFTERGLOW_PANE_ID` of the programs, which holds it.
Their `AFTERGLOW_SESSION` still names the session where the pane started, because a process cannot change the environment of another.

| Item of the pane | After the move to a new session |
| --- | --- |
| The title that you gave to the pane | Stays |
| Notify When the Next Command Finishes | Stays |
| The clipboard reads that you allowed for a password (OSC 5522) | Stay |
| What scripts set with `override` | Stays |
| The pointer shape of the program, and the keys that you hold down | Stay |
| Docked areas of the pane | Close. A script can dock them again when it gets `on_pane_created`. |
| An overlay | Ends, with the error `The pane under the overlay moved to another session` |
| A script dialog of the pane | Closes. The script gets the answer of a cancelled dialog. |
| File transfers, drags and downloads, with their Save dialogs | End |

These moves are not possible:

| Move | Message |
| --- | --- |
| The only pane of a session to a new window | `This pane is the only pane of session NAME, and it stays in its window` |
| A tab that holds all the panes of a session to a new window | `This tab holds all the panes of session NAME, and they stay in its window` |
| A pane of a session into another window | `A pane of session NAME cannot leave its window` |
| A pane of another window into the window of a session | `A pane cannot move into the window of session NAME` |

A move that is not possible shows a notice. `afterglow ctl` and the script effects fail with the message.
The palette does not show **Detach Tab to New Window** and **Move Pane to New Window** when they are not possible.
The tab menu then shows **Detach Tab to New Window** as not available.

The daemon moves the panes to the new session before a window opens for it.
When no window can show the new session, the move fails with `the pane went to session NAME, but no window can show it (REASON)`.
For a tab, the message starts with `the tab`.
The new session keeps running, and `afterglow mux attach NAME` shows it.

### Session panes

These features work as in a pane outside a session:

- The marks of shell integration (OSC 133): the last output, the jumps between prompts, and a click on the prompt
- Program status (OSC 7501), and the shell contexts (OSC 3008) in the title
- Extra cursors
- Images (Kitty graphics, Sixel, iTerm2) and their animations. The list below gives the differences.
- A selection or a search match in a region that a program scrolls, as an editor does: it moves with its text
- Visibility reports (mode 2033): the program hears whether you can see the pane

You can see a pane when its window shows on the screen and is not minimized, and the pane is in the current tab.
A pane that another pane hides with its zoom cannot be seen.
While the pane overview is open, you can see only the panes that it shows as cards.

A session pane differs from a pane outside a session in these ways:

- The window of a session shows only the panes of that session (see [move panes and tabs](#move-panes-and-tabs)).
- A Sixel or iTerm2 image does not show when the program prints it before any window shows the session. A Kitty image that the program places then does not move the cursor. The daemon places images in cells, and only a window measures a cell.
- An image shows when all of its pixels came from the daemon. The pixels of a large image come in more than one frame, so the image can show a short time after the text around it.
- A bell, a notification or another request of the program comes after the text on the screen that the program wrote before it. It can come before history and before the pixels of an image that the program sent before it.
- When a program sends an image again before its pixels came, the window shows each version that came whole.
- While you cannot see a pane, the daemon sends its window only these items, when they change:
  - The title and the working directory
  - The modes and the colors
  - The program status, the shell contexts and the marks of shell integration
  - The extra cursors
- The screen with its cursor, the history and the images of a pane that you cannot see wait in the daemon. They come when you can see the pane again. Until they come, the pane shows its older screen. They also come before each bell, notification or other request of the program, and before the program exits. Thus the window does not get the rows that a program in a background tab writes until it needs them.
- Before a read of the text of a pane that you cannot see, the window gets its screen and history from the daemon. It does this also for a pane that shows again before its screen came, and before a screenshot. The window waits for them for a limited time (see [limits](#limits)).
  A read of the selection does this too. The read gets at least the text that the pane had when the window asked. It can also get text that the program wrote after that.
- A pane outside a session decodes images with Qt, and a session pane with the decoders of its daemon. They show some images in a different way:
  - A pane outside a session shows a WebP image only when the Qt Image Formats plugin is installed. A session pane always shows it.
  - A pane outside a session shows a compressed BMP (run-length encoding) and a GIF whose screen has an empty side. A session pane does not show them.
  - A session pane shows a BMP with a header of 52 or 56 bytes, or with its masks after the header (compression 6). A pane outside a session does not show it.
  - Where the first frame of a GIF does not draw, a session pane shows nothing, as Kitty and browsers do. A pane outside a session can show the background color of the GIF there.
- The session keeps the terminal settings and the colors of the `afterglow mux new` that started it (see [settings of a session pane](#settings-of-a-session-pane)).

### Exit status

| Status | Meaning |
| --- | --- |
| 0 | The command did its work. `afterglow mux ls` without sessions also exits with 0. |
| 1 | The command failed and wrote why on standard error. Examples: no session has the name, the daemon did not start, or the command has the `--ssh` option. `afterglow mux kill` also exits with 1 when you do not answer `y`, `Y` or `yes` to its question. |
| 2 | A usage error, for example an unknown command, option or argument, or an option without its value. The command also writes the usage on standard error. |
| 2 | A session name that is not valid, for `afterglow mux new --name`, `attach` or `detach`. The command writes only the rule for names. |
| 126 | `afterglow mux server` found `afterglow-mux` but cannot run it. `afterglow mux new` or `attach` found `afterglow` but cannot run it. |
| 127 | `afterglow mux server` cannot find `afterglow-mux` next to `afterglow`. `afterglow mux new` or `attach` cannot find `afterglow`. |

The usage errors include these:

- No command, or an unknown command
- An argument to `ls` or `server`
- `afterglow mux kill` without a session name
- `afterglow mux detach` without a session name, outside a session pane (`AFTERGLOW_SESSION` is not set)

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
A new tab or split of the window starts from that environment too, not from the environment of the window's Afterglow.
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

The daemon sets these variables in each session pane:

| Variable | Value |
| --- | --- |
| `AFTERGLOW_SOCKET` | The first socket on which the daemon listens and that still exists when the session starts (see [sockets](#sockets)) |
| `AFTERGLOW_PANE_ID` | The pane ID in the daemon |
| `AFTERGLOW_SESSION` | The name of the session where the pane started (see [move panes and tabs](#move-panes-and-tabs)) |

The daemon removes `AFTERGLOW_OVERLAY`.
`Terminal.env` cannot set `AFTERGLOW_SESSION` or `AFTERGLOW_OVERLAY`.
An `afterglow` that starts from a session pane does not give `AFTERGLOW_SESSION` to its own panes.

`afterglow mux new` also sends the terminal settings of your configuration, such as `Terminal.scrollback` and the theme colors.
The session keeps these values (see [settings of a session pane](#settings-of-a-session-pane)).
`afterglow mux new` reads no colors from the application palette.
When `Colors.theme` names a dark and a light theme, the session gets the dark theme.
With a `plasma` theme, the session gets only the colors that `kdeglobals` sets, over the theme.
When `kdeglobals` sets no background, `plasma` alone takes `afterglow-dark`.

### Settings of a session pane

A session pane takes some settings from its session, and others from the window that shows it.

| Settings | From | When a change applies |
| --- | --- | --- |
| `Terminal.scrollback`, `Terminal.cursorShape`, `Terminal.cursorBlink`, `Terminal.ambiguousWide`, `Terminal.allowScreenReadback`, `Terminal.graphicsFileTransfer`, `Graphics.quota` | The session: the values of the `afterglow mux new` that started it. A new tab or split of the window gets them too. | Never for a running session |
| `Colors.theme`, `Colors.foreground`, `Colors.background`, `Colors.cursor`, `Colors.cursorText`, `Colors.selectionForeground`, `Colors.selectionBackground`, `Colors.palette` | The session, as above | Never for a running session |
| `Terminal.shell`, `Terminal.env`, `Terminal.term`, `Terminal.shellIntegration`, `Terminal.sshIntegration`, `Terminal.sshTerminfo` | The window, when it starts a new tab or split. The program starts in the environment of the session. | At the next new tab or split |
| `Terminal.wordSeparators`, `Colors.boldIsBright`, `Colors.minimumContrast` | The window | At once |
| `Terminal.bidi` | Both. The session decides which directional controls stay as visible cells, with the value of the `afterglow mux new` that started it. The window decides the order in which the text shows. | The order at once. The directional controls never for a running session. |
| `Terminal.scrollback` of the window | The window: the number of history rows that it shows (see [limits](#limits)) | At the next attach |
| `Terminal.bell`, `Terminal.clipboardRead` and the other settings of requests | The window (see [what a window answers](#what-a-window-answers)) | At once |

### Control in a session pane

The control socket of a window that shows a session lists the session panes, and acts on them as on the panes of other windows.
Text that `afterglow ctl send-text` sends to it goes to the program of the session.
A session that no window shows has no pane in a window, and no control socket of a window lists its panes.
A pane ID in the daemon and a pane ID in a window can have the same value.

In a session pane, `afterglow ctl`, `aglow run` and `afterglow ssh` connect to the daemon through `AFTERGLOW_SOCKET`.
The daemon answers each call and each subscription to the `control` and `commands` targets with the error `CALL_FAILED`, also while a window shows the session.
The message is `a session pane takes no control calls: the mux daemon answers only mux calls`.
Thus `afterglow ctl`, `afterglow ctl watch` and `aglow run` fail with that message.
`afterglow ssh` connects without [remote commands](CONTROL.md#remote-commands). With `-v`, it writes that message as the reason.

## What the daemon answers

The daemon reads the PTY output and parses it into the terminal of the pane, whether or not a window shows the session.
While no window shows it, the session keeps running, scrollback grows to the `Terminal.scrollback` limit, and the programs do not pause.
While an image of a pane decodes, the daemon reads no more output of that pane. A program that writes a lot then waits, as in a pane outside a session.
The daemon answers these requests itself, also while a window shows the session.
While a window shows the session, these answers go through the window, as the answers to queries do (see [what a window answers](#what-a-window-answers)).

| Request of a program | Result |
| --- | --- |
| Title and working directory | The daemon keeps the last value |
| Notification query (OSC 99 `p=?`) | The daemon gives the answer of a window |
| Query of the socket of an ssh session (OSC 24569) | The daemon answers that there is none (`error=off`) |
| Visibility report (mode 2033) | The window says whether you can see the pane. Until a window says it, the program hears that you cannot see the pane. This is also true after a takeover, and while no window shows the session. |
| Images | The daemon keeps the images, also while no window shows the session. It decodes PNG (Kitty graphics `f=100`) and iTerm2 images (PNG, JPEG, GIF, WebP and BMP). Each image decodes in a process of its own, which the decoder of the daemon (`afterglow-mux --decode-images`) starts. It decodes with libpng, libjpeg-turbo and libwebp, which the build includes. Each process has limits on its memory, its CPU time and the system calls that it can make. A decoder process that crashes or takes too long ends, and the daemon and the other sessions go on. The images do not decode on a kernel without seccomp filters, or on a kernel older than Linux 5.3 (`pidfd_open`). They also do not decode on a processor other than x86-64, little-endian AArch64, RISC-V 64 and little-endian POWER. The daemon then writes why on its standard error, which is the journal when it runs in `afterglow-mux.service`. When the decoder cannot start, images do not decode for a minute, and the daemon writes why. The window shows the images that the daemon keeps. |

While no window shows the session, the daemon also answers these requests.

| Request of a program | Result |
| --- | --- |
| Queries that the terminal answers itself (DA1, DSR, color queries) | The daemon answers |
| Query of the notifications on screen (OSC 99 `p=alive`) | The daemon answers with an empty list, because no notification is on screen |
| Bell, flash, notification, clipboard write | The daemon drops it. A clipboard write that asks for a status (OSC 5522) gets `EPERM`. |
| Clipboard read (OSC 52 and OSC 5522) | Refused, as when you deny the request |
| File transfer (OSC 5113) | Refused with `EPERM`, as when you deny the request. A transfer with `q=2` asks for no status and gets no answer. |
| Drag and drop (OSC 72) | A program that asks which types the terminal supports gets the answer of a window. A drag offer (`t=o`) gets `EPERM`. No drop arrives, and no drag starts. |
| File download (OSC 1337 `File`) | Dropped |
| Pointer shape (OSC 22), user variables (OSC 1337 `SetUserVar`), commands of shell integration (OSC 133) | The daemon keeps the last pointer shape, the user variables and the running command for the next window |

## What a window answers

While a window shows the session, the daemon sends the other requests of the programs to the window.
The window answers them as for a program of its own.
Your settings and prompts apply, such as the questions before a clipboard read and a file transfer.

| Request of a program | Result |
| --- | --- |
| Bell (BEL) | The window rings the bell, as `Terminal.bell` sets |
| Flash (OSC 555) | The window flashes the pane |
| Queries that the terminal answers itself (DA1, DSR, color queries) | The daemon makes the answer. The window writes it after its replies to the requests that came before the query, as for its own panes. |
| Notification (OSC 9, OSC 777, OSC 99) | The window shows it. A query of the notifications on screen (OSC 99 `p=alive`) gets the answer of the window. |
| Clipboard write and read (OSC 52 and OSC 5522) | The window writes or reads its clipboard, and asks first as for its own panes |
| File transfer (OSC 5113) and drag and drop (OSC 72) | The window runs them, with its questions |
| File download (OSC 1337 `File`) | The window asks where to save the file |
| Pointer shape (OSC 22) and user variables (OSC 1337 `SetUserVar`) | The window takes them as from its own panes. A window that attaches gets the last values. |
| Commands of shell integration (OSC 133) | The window takes them as from its own panes, with the time that the daemon measured. A window that attaches while a command runs gets its start. |

### Requests that wait

The requests wait for the window in a queue of the session and in a queue of each pane.
`Mux.eventQueue` sets the size of each queue. A change applies when Afterglow next attaches to a session.
A request that finds the queue of its pane empty always fits, up to the limit of one request.
A request that does not fit gets the answer of a session that no window shows (see [what the daemon answers](#what-the-daemon-answers)).
Thus a notification, an OSC 52 clipboard write and a download that do not fit are dropped.
These requests are exceptions:

- An answer to a query that does not fit is dropped, as a window drops one past its own limit.
- When commands of file transfer and drag and drop do not fit, the window ends the transfer or the drag, as for its own panes.

These requests do not take more room when more come:

- Bells and flashes that wait count once.
- The pointer shape and each user variable wait with their last value only.
- The end of a command always goes when its start went.

When the window goes, the requests that still wait get the answer of a session that no window shows.
A request that the window got already gets no answer from the daemon.
A clipboard read that waits for your answer gets the denial when its window closes and the session keeps running.
When a session pane closes in the window, a file transfer that the window ran gets no more answers.
A pane can move to a new session while a request of its program waits for the window. That request gets the answer of a session that no window shows.

## Limits

| Item | Limit | At the limit |
| --- | --- | --- |
| Session name | 1 to 64 characters: ASCII letters, digits, `_` and `-`. The first character is an ASCII letter, a digit or `_`. | `afterglow mux new` refuses the name with exit status 2 |
| Socket path | 107 bytes | The daemon and the client leave out a socket with a longer path. When no socket is left, they stop and name the path. |
| Open files | The hard limit of `RLIMIT_NOFILE`. The daemon raises its soft limit to the hard limit when it starts. | A new connection closes at once. A new session gets an error that says why its program cannot start. A PNG or iTerm2 image does not decode. |
| Request line | 1 MiB, with its newline | The daemon answers `MALFORMED_REQUEST` and closes the connection |
| Lines that do not parse, one after the other | 16 | Each line gets `MALFORMED_REQUEST`. The daemon closes the connection after its answer to the 17th line. |
| Waiting output to one client | 32 MiB | The daemon closes the connection to a client that does not read its answers |
| Environment, arguments and directory of a session | UTF-8 text | `afterglow mux new` changes each byte that is not UTF-8 to U+FFFD |
| Wait for a window to connect to its attach socket | 5 seconds | The daemon removes the socket, and `afterglow mux attach` stops with exit status 1 |
| Input of a window that a program does not read | 32 MiB in the queue of the PTY of the pane. Then the daemon holds the input of the pane, up to 4 MiB for all the panes of the window. Then 32 MiB for each pane in the window. | While the daemon holds less than 4 MiB, the other panes still get their input. At 4 MiB, the daemon stops reading the input of the window. The window sends which panes you can see, and its reads of the screen and the history of a pane, apart from its input. Input that waits does not stop them. At 32 MiB for one pane, the window refuses more input for that pane and shows a notice, as for a pane of its own. Closing the window with `Mux.onClose` kill still ends the session. |
| Frames to a window | About 2 MiB of state in each frame, and at most 4 MiB, the largest frame of the stream | Larger state goes in more frames, so a slow window gets fewer frames. It never holds a queue of old ones. A pane that you cannot see gets frames without its screen. |
| History rows to a window | `Terminal.scrollback` of the window when it attaches, in rows | The daemon sends no older rows |
| Wait for the screen and the history of a pane that you cannot see, or that shows again before its screen came, before a read of its text or a screenshot | 5 seconds | The read gets the text that the window has, which can be older. The window does not draw or take input while it waits. |
| Image file that the daemon decodes | The limits of a pane outside a session (see [COMPATIBILITY.md](COMPATIBILITY.md)): 320 MiB of Kitty data, and 96 MiB of iTerm2 base64 | The error of a pane outside a session: `EFBIG` for data in the command, `EBADF` for a file or shared memory. An iTerm2 image does not show. |
| Image that the daemon decodes | 10,000 pixels on a side, and 320 MiB of pixels | A Kitty command gets an `EBADPNG` error. An iTerm2 image does not show. |
| Time to decode an image | 30 seconds, and a CPU time of 5 seconds and 1 second more for each 16 MiB of pixels | The decoder process ends. The image does not show, as above. The daemon writes a line on its standard error, at most once a minute. |
| Memory of a decoder process | Its memory at its start, and 8 times the decoded pixels and 64 MiB more | The decode fails. The image does not show, as above. |
| Scans of a progressive JPEG | 1,000 | The image does not show, as above. |
| Panes of a session | 256 | A new tab or split fails with `session 'NAME' has 256 panes, the most that a session can have`. The window shows `Could not open a tab` or `Could not split the pane` with that message. `afterglow ctl` and scripts get it as the error. |
| Title of a tab or a pane that the daemon keeps | 1,024 bytes of UTF-8 | The window sends the start of a longer title, up to the end of a whole character. |
| Memory of the daemon for a window that does not read | For each pane: the frames that its channel takes (4 MiB), one frame more, and the requests in the queue of the pane (`Mux.eventQueue`). For the window: the queue of the session (`Mux.eventQueue` and one request), and the input that the daemon holds (4 MiB). | The daemon makes no newer frame of a pane until its waiting frame went. It never waits for a window. |
| Region scrolls in one frame | 256 | Every row from the top of the highest region that scrolled to the bottom of the lowest leaves its region. A selection or a search match in those rows goes. |
| Requests that wait for a window | `Mux.eventQueue` (1 to 64 MiB, 4 MiB by default) in the queue of the session, and as much in the queue of each pane. A request that finds the queue of its pane empty passes the limit. The queue of the session also takes one request of any size while it is below the limit. | The answer of a session that no window shows (see [requests that wait](#requests-that-wait)) |
| One request to a window | 129 MiB: the largest clipboard write that a terminal takes, and its types | The answer of a session that no window shows |

The daemon sets no limit on the number of sessions or connections. Each session and each connection uses open files.
