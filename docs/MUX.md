<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# The mux daemon

The mux daemon, `aglow-mux`, keeps terminal sessions running while no window shows them.
A session is a set of tabs and split panes with running programs. A session that `afterglow mux new` creates starts with one pane.
The `afterglow mux` commands create, list and end sessions. They also show a session in a window, and close that window again.

The mux is opt-in. A normal launch of Afterglow does not use it.
A mux session and a normal `afterglow` launch can run at the same time.

## The daemon

`aglow-mux` is a daemon with no Qt. It needs no display and no GPU.
A second process of it starts a process for each image that a session decodes (see [what the daemon answers](#what-the-daemon-answers)).
It owns the sessions, the PTYs of their panes and the terminal state of each pane.
It reads no configuration file. The Afterglow that starts a session sends the values that the session needs (see [environment](#environment)).

On this computer, `afterglow mux` runs the `aglow-mux` in the directory of the `afterglow` executable. It does not search `PATH`.
With `--ssh`, the shell of your login on the host finds the program that `Mux.remoteCommand` names (see [sessions on another host](#sessions-on-another-host)).

### Start

`afterglow mux new` starts the daemon when no daemon answers on a [socket](#sockets).
`afterglow mux attach` without `NAME` starts it too when no session runs, because it then creates a session.
A [project](PROJECTS.md#projects-and-mux-sessions) that opens into a session starts it too, from **Switch Project**, `afterglow ctl open-project` or a script.
`afterglow mux new --ssh` and `aglow-mux proxy --start` start the daemon of another host (see [sessions on another host](#sessions-on-another-host)).
`afterglow mux attach NAME`, `afterglow mux detach`, `afterglow mux ls`, `afterglow mux kill` and `afterglow ctl --session` do not start the daemon.

The command uses the systemd user manager when all of these conditions are true:

- `XDG_RUNTIME_DIR` is `/run/user/UID`.
- The systemd user manager runs.
- Lingering is on for your user, or `XDG_SESSION_TYPE` is `wayland` or `x11`.

| Condition | How the daemon starts |
| --- | --- |
| All of the conditions are true | In its own systemd user unit, `aglow-mux.service`, with `systemd-run --user` |
| One of the conditions is false | In a new session with `setsid`, outside the process group and the terminal of the command |
| `systemd-run` fails, and `aglow-mux.service` does not exist | In a new session with `setsid` |
| `systemd-run` fails, and `aglow-mux.service` exists | No second daemon starts. The command waits for the daemon of that unit, which starts or ends. |

`UID` is your user ID. Lingering is on when `/var/lib/systemd/linger/USER` exists (`loginctl enable-linger`).
The command finds the user manager through its socket, `$XDG_RUNTIME_DIR/systemd/private`.

`systemd-run` gives the daemon the `XDG_STATE_HOME` and `HOME` of the command, so that the daemon and its clients agree on the second socket.

The unit of the daemon is not the unit of a window.
When systemd-oomd stops the unit of a window, the sessions stay.
Without lingering, the user manager stops its units at your last logout.
For this reason, an SSH login without lingering starts the daemon with `setsid`.
A daemon that starts with `setsid` stays after your last logout, unless logind sets `KillUserProcesses=yes`.

`afterglow mux new` waits up to 5 seconds for a socket of the daemon to answer.
When the user manager does not answer, the command stops waiting after these 5 seconds.
A last look at the unit and at the sockets can take 1 second more.
When the daemon does not start, the command stops with exit status 1.
It writes `afterglow mux new: ` and a message on standard error.
When `afterglow mux attach` creates the session, the message starts with `afterglow mux attach: `.

| Start | Result | Message |
| --- | --- | --- |
| `setsid` | The daemon reports an error before it exits, for example when it can use none of its sockets (see [sockets](#sockets)) | The error of the daemon, such as `no socket to listen on: ` and the reason for each socket. The command stops at once. |
| `setsid` | The daemon finds a socket that another daemon holds, and that daemon does not answer in 5 seconds | `aglow-mux: another aglow-mux listens on PATH, which does not answer` |
| `setsid` | The command cannot run `aglow-mux` | `aglow-mux: cannot run PATH: REASON`, where `REASON` is the error text of the system. The command stops at once. |
| `setsid` | The daemon exits before it listens and reports no error | `aglow-mux ended before it listened`. The command stops at once, without the wait of 5 seconds. |
| `setsid` | No socket of the daemon answers in 5 seconds | `aglow-mux did not start: no daemon answers on PATH`, with the path of each socket |
| systemd user unit | The unit is in the state `deactivating`, and no socket answers in 5 seconds | `aglow-mux did not start: the daemon of aglow-mux.service is ending. Try again when it has ended` |
| systemd user unit | No socket of the daemon answers in 5 seconds. Or the unit that the command started ends before its daemon answers, and no other daemon holds the lock of a socket. | `aglow-mux did not start: see journalctl --user -u aglow-mux`. The journal shows the error of the daemon. |
| systemd user unit | Another daemon holds the lock of a socket and does not answer in 5 seconds. A lock file that a killed daemon left holds no lock. | `aglow-mux did not start: another aglow-mux holds a socket and does not answer` |
| `systemd-run` fails, and a unit file `aglow-mux.service` exists that does not run | No daemon starts. | `aglow-mux did not start: the user unit aglow-mux.service exists and does not run. Start it with systemctl --user start aglow-mux.service, or remove it` |
| `systemd-run` or `systemctl` does not end in 5 seconds, or the last look at the unit does not end in 1 second more | No daemon answers at the last look at the sockets. | `aglow-mux did not start: the systemd user manager does not answer` |

Two commands can start a daemon at the same time. Then the second daemon finds the socket of the first daemon and exits.
Its command waits for the first daemon, because the first daemon can take some time before it answers.
When the unit `aglow-mux.service` is still loaded because its daemon ends, `afterglow mux new` waits for the end of that unit.
When the unit is gone, the command starts the unit again one time, in the same 5 seconds.

A daemon of another build of Afterglow can speak another protocol.
Then `afterglow mux new`, `attach`, `detach`, `ls` and `kill` stop with exit status 1, and no window opens.
The message is `aglow-mux speaks protocol N, and this program speaks M: end its sessions with the afterglow that started it, or wait until they end`.
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
| `afterglow mux new` | Creates a session and shows it in a window. Options: `--name NAME`, `--cwd DIR`, `--detach`, `--ssh DESTINATION`, `-- COMMAND...`. |
| `afterglow mux attach [NAME]` | Shows a session in a window. Options: `--no-takeover`, `--ssh DESTINATION`. |
| `afterglow mux detach [NAME]` | Closes the window of the session. The session keeps running. Option: `--ssh DESTINATION`. |
| `afterglow mux ls` | Lists the sessions, with the columns `NAME`, `STATE` and `PANES`. Option: `--ssh DESTINATION`. |
| `afterglow mux kill NAME` | Ends a session and its programs. Options: `--force`, `--ssh DESTINATION`. |
| `afterglow mux server` | Runs `aglow-mux` in the foreground |
| `afterglow mux --help` | Prints the usage |

`afterglow mux new` shows the new session in a window. With `--detach`, it creates the session without a window and prints its name.
The `afterglow mux` commands read the settings of the default config file, `$XDG_CONFIG_HOME/afterglow/config.json`, not the `--config` or `-o` of a running Afterglow.
The session starts in `DIR`, or in the current directory without `--cwd`. A directory that does not exist stops the command.
Without `COMMAND`, the session runs the program of a new pane in a window: `Terminal.shell`, else your login shell.
The pane has the size of a new window: `Window.initialColumns` by `Window.initialRows` cells.
Without `--name`, the daemon gives the session a number as its name, from `1`.
The daemon does not give the same number two times while it runs. A number that a session name has is skipped.
A name that a session has already is refused.

`afterglow mux ls` shows `attached` in the `STATE` column when a window shows the session, else `detached`.
The `PANES` column shows the number of panes in the session.
Without a daemon, `afterglow mux ls`, `afterglow mux detach` and `afterglow mux kill` report that there are no sessions.

When `Terminal.confirmClose` is on and a program runs in the session, `afterglow mux kill` asks first.
It asks only when its standard input is a terminal.
Without a terminal, it refuses and names `--force`.
The command itself is not a running program when it runs in the foreground of a session pane of that session.
`--force` ends the session without a question.

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
| You close the window, or its last pane or tab | `Mux.onClose`: `kill` (the default) ends the session and its programs, as in a window outside a session. `detach` keeps the session running, with its tabs and splits. A window that a [project](PROJECTS.md#projects-and-mux-sessions) opened into its session always keeps it, also with `--new`. A save under a project name does not change this. |
| You quit Afterglow | The session keeps running, whatever `Mux.onClose` says. When a window stops the quit with a question, `Mux.onClose` applies again to the windows that stay open. At your last logout, the daemon can end too (see [start](#start)). |
| `afterglow mux detach NAME` | The window closes. The session keeps running. In a session pane, `NAME` defaults to the session that holds that pane (see [detach in a session pane](#detach-in-a-session-pane)). With `--ssh`, `NAME` is necessary. |
| Another window shows the session (`afterglow mux attach`) | The window closes, with a notice. The session keeps running. |
| The window crashes, or its connection to the daemon breaks | The session keeps running. A window that still runs closes, with a notice. |
| The program of a pane exits | The pane shows the last output of the program, then closes as a pane does in a window outside a session. After the last program of the session, the session ends. |
| `afterglow mux kill NAME` | The session ends, and its window closes |

With `Mux.onClose` at `detach`, closing the window, or its last tab or pane, asks nothing about the programs of the session.
The same is true for a window that a project opened into its session.
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
This is true for the keys, the menu, scripts and `afterglow ctl` through the socket of the window.
The program of the new pane runs under the daemon, with the environment of the session (see [environment](#environment)).
The window prepares the program with its own settings, such as `Terminal.shell` and `Terminal.env`.
`afterglow ctl` through the daemon prepares it with the settings of the default config file (see [control of a session](#control-of-a-session)).
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
| Commands | The command that started each pane, for a [project](PROJECTS.md#projects-and-mux-sessions) that saves the window, or none for the shell. A window of a session of another host keeps no commands, because a project runs its commands on this computer. |

While no window shows the session, a pane whose program exits leaves its tab, as in a window.

New Window (`new_window`) in the window of a session opens a window outside the session.
Its first pane starts in the directory that a new tab of the focused pane gets (see [working directory](#working-directory)).
Its programs end with that window, as in any window outside a session.

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
A pane can also join another session (see [move a pane to another session](#move-a-pane-to-another-session)).
A pane or a tab that you move to a new window leaves the session. These moves take a pane or a tab to a new window:

- A drag of a tab out of the tab bar
- **Detach Tab to New Window** in the tab menu, and the `detach_tab` action
- **Move Pane to New Window**, and the `move_pane_to_window` action
- `afterglow ctl move-pane 0 new-window` and `afterglow ctl detach-tab`
- The script effects `move_pane` with the placement `new-window`, and `detach_tab`

The programs of the pane or the tab go on in a new session, which gets the next number as its name.
A new window shows that session.
Until the new window shows the pane, the pane stays in its window, and another move of it fails.
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
| The clipboard reads and writes that you allowed for a password (OSC 5522) | Stay |
| What scripts set with `override` and `status` | Stays |
| The shader signals of the pane, from `afterglow ctl signal` and scripts | Go back to 0 |
| The shader parameters that scripts set for the pane (`shader_parameters`) | Go |
| A script key mode over the pane | Stays in the old window, without the pane |
| The pointer shape of the program, and the keys that you hold down | Stay |
| Docked areas of the pane | Close. A script can dock them again when it gets `on_pane_created`. |
| An overlay | Ends, with the error `The pane under the overlay moved to another session` |
| A script dialog of the pane | Closes. The script gets the answer of a canceled dialog. |
| File transfers, drags and downloads, with their Save dialogs | End |
| The question of an upload (OSC 1337 `RequestUpload`), with its file dialog | Closes in the old window. The program still waits. The next window that shows the pane asks again, while the job that asked still holds the terminal. |

These moves are not possible:

| Move | Message |
| --- | --- |
| The only pane of a session to a new window | `This pane is the only pane of session NAME, and it stays in its window` |
| A tab that holds all the panes of a session to a new window | `This tab holds all the panes of session NAME, and they stay in its window` |
| A pane of a session into a window that shows no session | `A pane of session NAME can move only into the window of another session` |
| A pane of another window into the window of a session | `A pane cannot move into the window of session NAME` |
| A pane of a session into the window of a session on another host | `A pane of session NAME cannot move to session OTHER, which runs on another host` |
| A pane of a session into the window of a session of another daemon on this computer | `A pane of session NAME cannot move to session OTHER, which runs under another mux daemon` |
| A pane into a session that has 256 panes | `session 'OTHER' has 256 panes, the most that a session can have` |

A move that is not possible shows a notice. `afterglow ctl` and the script effects fail with the message.
The palette does not show **Detach Tab to New Window** and **Move Pane to New Window** when they are not possible.
The tab menu then shows **Detach Tab to New Window** as not available.

The daemon moves the panes to the new session before a window opens for it.
The windows go on responding while the daemon moves the panes and the new window attaches.
When no window can show the new session, the move fails with `the pane went to session NAME, but no window can show it (REASON)`.
For a tab, the message starts with `the tab`.
The new session keeps running, and `afterglow mux attach NAME` shows it.

### Move a pane to another session

A pane of a session can join another session of the same daemon, as `tmux join-pane` does.
Both sessions must run on this computer, or both on the same host.
These moves take a pane into another session:

- `afterglow ctl move-pane TARGET PLACEMENT` through the socket of a window, with a pane `TARGET` in the window of the other session. `TARGET` is a pane ID of the windows.
- The script effect `move_pane`, with a target pane in the window of the other session
- `afterglow ctl --session NAME move-pane TARGET PLACEMENT`, with a pane `TARGET` of another session. `TARGET` is a pane ID in the daemon.
- `afterglow ctl move-pane TARGET PLACEMENT` in a session pane, with a pane `TARGET` of another session. `TARGET` is a pane ID in the daemon.

| Placement | Where the pane goes |
| --- | --- |
| `left`, `right`, `up`, `down` | Beside the target pane, in half of its room |
| `new-tab` | In a new tab after the tab of the target pane |

When a window shows the other session and the target pane has no room for the split, the pane goes in a new tab after the tab of the target pane.

The daemon moves the pane with its program, its history and its terminal state. The program keeps running, with the same process.
The window of the other session shows the pane. When no window shows that session, the next window shows the pane.
When the pane was the last pane of its session, that session ends, and its window closes.
The windows go on responding while the daemon moves the pane.
Until the daemon has moved the pane, the pane stays in its window, and another move of it fails.

The pane gets a new pane ID in the window, as for a move to a new session.
The pane ID in the daemon stays the same, and so does `AFTERGLOW_PANE_ID`.
`AFTERGLOW_SESSION` still names the session where the pane started.
The pane takes the terminal settings of the other session.
After a move from a window, the items of the pane stay or go as the table for a move to a new session says.
After a move by `afterglow ctl --session`, the window of the first session closes the pane with no notice.
The title that you gave to the pane goes with it. Other things that the window kept for the pane go.
`afterglow ctl --session` also needs `Control.enabled` of the other session. Else the move fails with `session 'OTHER' takes no control calls (Control.enabled)`.
When no window shows the other session, the target pane needs 4 columns for `left` and `right`, and 2 rows for `up` and `down`. Else the move fails with `There is not enough room beside that pane`.

A pane comes into a window once at a time.
A pane that left a window goes back into it only after that window let it go, which takes a moment.
Until then, a move of it back fails with `pane N is still leaving the window of session 'NAME'`.
A pane that came into a window a moment ago moves only after its window shows it.
Until then, a move of it fails with `This pane is still arriving in session NAME`.
Try the move again.

When the window of the other session closed during the move, the move fails with `the pane went to session NAME, but its window no longer shows it`.
When that window cannot place the pane, the move fails with `the pane went to session NAME, but its window cannot show it`.
In both cases, the pane is in session `NAME`, and `afterglow mux attach NAME` shows it.

### Session panes

These features work as in a pane outside a session:

- The marks of shell integration (OSC 133): the last output, the jumps between prompts, and a click on the prompt
- Program status (OSC 7501), and the shell contexts (OSC 3008) in the title
- Extra cursors
- Images (Kitty graphics, Sixel, iTerm2) and their animations. The list below gives the differences.
- The decode of images. A session pane uses the decoders of its daemon. A pane outside a session uses the same decoders in processes of the window. Both show the same images alike.
- A selection or a search match in a region that a program scrolls, as an editor does: it moves with its text
- Visibility reports (mode 2033): the program hears whether you can see the pane

You can see a pane when its window shows on the screen and is not minimized, and the pane is in the current tab.
A pane that another pane hides with its zoom cannot be seen.
While the pane overview is open, you can see only the panes that it shows as cards.

A session pane differs from a pane outside a session in these ways:

- The window of a session shows only the panes of that session (see [move panes and tabs](#move-panes-and-tabs)).
- Before any window shows the session, its cells have the size of the font of your settings (`Font`).
  The scale is that of the primary screen when Afterglow starts the session, else 1.
  The daemon places images in cells with that size. A window then measures its own cells, and an image that the program placed before keeps its cells.
  When the font does not load, the cells have no size. Then a Sixel or iTerm2 image does not show, and a Kitty image does not move the cursor, until a window shows the session.
- A program in a session pane cannot change the size of its pane. The window that shows the session sets it.
  The daemon ignores DECCOLM, DECSCPP, DECSNLS, DECSLPP and the resize requests of XTWINOPS. It does not answer `CSI 15 t` and `CSI 19 t`.
- The window encodes keys, focus reports, mouse reports and pastes with the modes that the program set, as the window last heard of them.
  Over a slow link, a key that comes right after a program changes its key mode can use the old mode.
  So can a paste that comes right after the program turns off bracketed paste.
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
- The session follows the terminal settings and the colors of the window that shows it.
  While no window shows it, it keeps those of its last window (see [settings of a session pane](#settings-of-a-session-pane)).

### Exit status

| Status | Meaning |
| --- | --- |
| 0 | The command did its work. `afterglow mux ls` without sessions also exits with 0. |
| 1 | The command failed and wrote why on standard error. Examples: no session has the name, the daemon did not start. `afterglow mux kill` also exits with 1 when you do not answer `y`, `Y` or `yes` to its question. |
| 2 | A usage error, for example an unknown command, option or argument, or an option without its value. The command also writes the usage on standard error. |
| 2 | A session name that is not valid, for `afterglow mux new --name`, `attach` or `detach`. The command writes only the rule for names. |
| 126 | `afterglow mux server` found `aglow-mux` but cannot run it. `afterglow mux new` or `attach` found `afterglow` but cannot run it. |
| 127 | `afterglow mux server` cannot find `aglow-mux` next to `afterglow`. `afterglow mux new` or `attach` cannot find `afterglow`. |
| 255 | `afterglow mux new --ssh` cannot connect to the host to install the terminfo entry. |

The usage errors include these:

- No command, or an unknown command
- An argument to `ls` or `server`
- `afterglow mux kill` without a session name
- `afterglow mux detach` without a session name, outside a session pane (`AFTERGLOW_SESSION` is not set)
- `afterglow mux detach --ssh DESTINATION` without a session name, also in a session pane
- `--ssh` without a destination, or `--ssh` more than once

A window that `afterglow mux attach` opens in an Afterglow that runs already reports its errors with the prefix `afterglow: `.

## Sockets

The daemon listens on each socket of this table that it can use.
A client tries the sockets in the order of the table and uses the first one that answers.

| Order | Directory | Socket in the directory |
| --- | --- | --- |
| 1 | `$XDG_RUNTIME_DIR/aglow-mux` | `socket` |
| 2 | `$XDG_STATE_HOME/aglow-mux`, by default `~/.local/state/aglow-mux` | `ID.sock` |

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
Then the new daemon exits with status 1 and the message `another aglow-mux listens on PATH`.
Your sessions stay in the daemon that holds the lock.
`afterglow mux new` then waits for the daemon that holds the lock (see [start](#start)).
A socket can fail for another reason, such as a directory with the wrong mode.
Then the daemon writes the reason on standard error and uses the other socket.
When the daemon has no socket, it exits with status 1.
When the daemon exits, it removes its sockets and its lock files.

A window that shows a session gets the state of the session on the connection of its attach call.
The daemon uses no other socket for it.

## Sessions on another host

`--ssh DESTINATION` acts on the daemon of another host, through ssh.
Each `afterglow mux` command but `server` takes it. `NAME` is the name of a session on that host.
`afterglow mux new --ssh` starts the daemon of the host when none runs there, and `attach --ssh` starts it when it creates a session.
The window of the session opens on this computer, as for a session of this computer.

The command runs `aglow-mux proxy` on the host over an ssh connection of Afterglow.
This connection is a master with the `ControlPath` `afterglow-ssh-%C` in `XDG_RUNTIME_DIR`.
`afterglow ssh` never uses this master. When your SSH configuration sets no `ControlPath`, `afterglow ssh` makes a master of its own (see [SSH](INPUT.md#ssh)).
Afterglow never uses a `ControlPath` or a `ControlMaster` of your SSH configuration for the sessions of another host.
The connection stays open for `Terminal.sshControlPersist` seconds after its last command, 10 by default.
It forwards no port of your configuration and runs no `LocalCommand`.
The proxy connects to the daemon there, and carries that connection on its standard input and output. ssh needs no port forward.

You sign in in the terminal of the command.
The command waits `Mux.signInTimeout` seconds (120 by default) for the first answer of the host. In that time, ssh can ask for a password or a passphrase.
The window of the session asks for nothing (ssh `BatchMode`). It uses the connection that the command made, or keys alone.
`XDG_RUNTIME_DIR` must be an absolute path of letters, digits, `/`, `.`, `_` and `-`.

The host needs the program that `Mux.remoteCommand` names, by default `aglow-mux`.
ssh runs it in your login shell on the host, without a terminal.
Thus the program must be on the `PATH` that this shell gets without a terminal, or `Mux.remoteCommand` gives its path there.
Your login files on the host must write nothing on standard output when the shell is not interactive.
The host does not need Qt or the rest of Afterglow: see [build aglow and aglow-mux alone](BUILD.md#build-aglow-and-aglow-mux-alone).
The daemon of the host must speak the protocol of this Afterglow.

| Result | Message |
| --- | --- |
| No daemon runs on the host | `afterglow mux ls` reports no sessions. `detach` and `kill` report `no sessions`. No daemon starts. |
| The host has no `aglow-mux`, and the host writes no error | `DESTINATION has no aglow-mux: install it there` |
| The host cannot run `aglow-mux`, for example because its shell does not find it or a library is missing | `DESTINATION cannot run aglow-mux: ERROR: install it there`, where `ERROR` is what the host wrote on its standard error |
| The login on the host writes on standard output | `DESTINATION wrote text that is not from aglow-mux: a login file of the shell there, such as .bashrc, .zshenv or config.fish, writes on standard output when the shell is not interactive` |
| The daemon speaks another protocol | `the aglow-mux of DESTINATION speaks protocol N, and this afterglow speaks M: use the same version of Afterglow on both computers` |
| ssh fails | What ssh wrote on its standard error |
| ssh cannot read the destination | `ssh cannot read the destination 'DESTINATION' (ssh -G)` |
| The destination is empty or starts with `-` | `'DESTINATION' is no ssh destination` |
| `XDG_RUNTIME_DIR` is not set, or is not such a path | `no private XDG_RUNTIME_DIR for the ssh connection of Afterglow: it must be an absolute path of letters, digits, '/', '.', '_' and '-'` |
| The host gives no first answer in `Mux.signInTimeout` seconds | The command stops with an error |

When `Mux.remoteCommand` is not `aglow-mux`, the two messages with `install it there` name it in parentheses: `aglow-mux (COMMAND)`.

`aglow-mux proxy` carries one connection to the daemon of its host on its standard input and output.
With `--start`, it starts the daemon of the host when none runs, as `afterglow mux new` does on this computer.
Without `--start`, it exits with status 3 when no daemon runs.

A session of another host runs with the environment of your ssh login there, which the proxy has, not with the environment of this computer.
`afterglow mux new --ssh` sets these variables over it:

| Variable | Value |
| --- | --- |
| `TERM` | `Terminal.term`. With `afterglow`, the command first installs the terminfo entry on the host, as `afterglow ssh` does. When this computer has no terminfo entry for `afterglow`, or the host has no `tic`, `TERM` is `xterm-256color`. It is also `xterm-256color` with `Terminal.sshTerminfo` at `fallback`. When ssh cannot connect for the install, the command stops with exit status 255. |
| `COLORTERM`, `TERM_PROGRAM`, `TERM_PROGRAM_VERSION` | As in a pane of this computer |

A pane of the session without a command runs your shell there as a login shell (`-l`).
The reason is that the environment of an ssh command did not read your login files.
It starts in your home directory there, or in `--cwd DIR`, which must be an absolute path on the host.
`Terminal.shell`, `Terminal.env` and shell integration of this computer do not apply, because they name paths of this computer.
A shell integration that you install on the host works there.

A new tab or split of the window of the session runs your shell there too.
It starts in the directory of the program in the foreground of the pane that it opens from.
A `cwd` or an `env` that `afterglow ctl` or a script gives for it is refused.
`afterglow ctl` and scripts read the directory of a pane of the session only when it also exists on this computer.
The [command output](INPUT.md#command-output) actions do not work in a window of a session of another host. These are `show_last_output`, `show_first_output_on_screen` and `show_last_visited_output`.
The pager runs on the host, and the output file is on this computer.

`Mux.remoteCommand`, `Mux.signInTimeout` and `Terminal.sshControlPersist` are the settings of these sessions:

| Setting | Function | Default |
| --- | --- | --- |
| `Mux.remoteCommand` | The command line that ssh gives your login shell on the host | `aglow-mux` |
| `Mux.signInTimeout` | The seconds that the `afterglow mux` commands wait for the first answer of the host, while ssh can ask for a password. A window waits 5 seconds. | 120 |
| `Terminal.sshControlPersist` | The seconds that the ssh connection of Afterglow stays open with no command on it | 10 |

When the ssh connection breaks, the window closes with a notice, and the session keeps running on the host.
`afterglow mux attach --ssh` shows it again.

## Environment

The programs of a session run with the environment of the Afterglow that started the session, not with the environment of the daemon.
That Afterglow is `afterglow mux new`, or the Afterglow that opened a [project](PROJECTS.md#projects-and-mux-sessions) into the session.
A new tab or split of the window starts from that environment too, not from the environment of the window's Afterglow.
Thus a session does not get variables of the daemon's systemd unit, such as `INVOCATION_ID`.
The Afterglow that started the session prepares the program as a window prepares a new pane.
It adds shell integration, `TERM`, `COLORTERM`, `TERM_PROGRAM` and the values of `Terminal.env`.
The programs of the session get the umask of that Afterglow.
They get the soft limit of open files that the daemon had before it raised its own (see [limits](#limits)).

Each time a window shows a session of this computer, the session takes the variables of `Mux.updateEnvironment` from that Afterglow, as tmux does with `update-environment`.
By default these are `DISPLAY`, `WAYLAND_DISPLAY`, `XAUTHORITY`, `SSH_AUTH_SOCK`, `SSH_AGENT_PID`, `SSH_CONNECTION`, `SSH_ASKPASS` and `KRB5CCNAME`.
Thus a session that you show again after you log in again gets the new `SSH_AUTH_SOCK`.

| Afterglow has the variable | The session |
| --- | --- |
| Yes | Takes Afterglow's value |
| No | Removes it |

Only the panes that start after that get the new values. A program that runs keeps its environment.
A session of another host keeps its own values, because the values of this computer do not apply there.

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

The Afterglow that started the session also sends the terminal settings of your configuration, such as `Terminal.scrollback` and the theme colors.
From then on, the session follows the settings of the window that shows it.
The window sends them when it shows the session, and again after each change, such as an edit of the configuration file.
The daemon gives them to each pane of the session, also to a pane that starts later.
While no window shows the session, it keeps the settings of its last window.
A change of the terminal settings keeps the colors that the programs set, as in a window.
These are the colors of OSC 4, 10, 11, 12, 17, 19 and 21.
A color that a program did not change takes the new value.
The same applies when a window with other terminal settings shows the session, such as an Afterglow with another configuration.
The requests that reach a window follow the settings of that window, such as `Terminal.bell` and `Terminal.clipboardRead` (see [what a window answers](#what-a-window-answers)).
A change of these settings applies at once.
`afterglow mux new` reads no colors from the application palette.
When `Colors.theme` names a dark and a light theme, `afterglow mux new` gives the session the dark theme.
A window gives it the theme that the window shows.
With a `plasma` theme, the session gets only the colors that `kdeglobals` sets, over the theme.
When `kdeglobals` sets no background, `plasma` alone takes `afterglow-dark`.

### Settings of a session pane

A session pane takes some settings from the session, which follows the window that shows it, and others from that window alone.

| Settings | From | When a change applies |
| --- | --- | --- |
| `Terminal.scrollback`, `Terminal.cursorShape`, `Terminal.cursorBlink`, `Terminal.ambiguousWide`, `Terminal.bidi`, `Terminal.allowScreenReadback`, `Terminal.graphicsFileTransfer`, `Graphics.quota` | The session: the settings of the window that shows it, else those of its last window, else those of the Afterglow that started it | At once, for each pane of the session |
| `Colors.theme`, `Colors.foreground`, `Colors.background`, `Colors.cursor`, `Colors.cursorText`, `Colors.selectionForeground`, `Colors.selectionBackground`, `Colors.palette` | The session, as above | At once. The colors that the programs set (OSC 4, 10, 11, 12, 17, 19 and 21) stay, as in a window. |
| `Terminal.shell`, `Terminal.env`, `Terminal.term`, `Terminal.shellIntegration`, `Terminal.sshIntegration`, `Terminal.sshTerminfo` | The window, when it starts a new tab or split. The program starts in the environment of the session. A session of another host does not use them. Its panes run your login shell there, with the `TERM` that `afterglow mux new --ssh` gave the session. | At the next new tab or split |
| `Terminal.wordSeparators`, `Colors.boldIsBright`, `Colors.minimumContrast` | The window | At once |
| `Terminal.bell`, `Terminal.clipboardRead` and the other settings of requests | The window (see [what a window answers](#what-a-window-answers)) | At once |
| `Mux.keptNotifications` | The session, as for the terminal values | At once, for each pane of the session |

### Control of a session

`afterglow ctl --session NAME COMMAND` acts on session `NAME` through the daemon, also while no window shows the session.
It reaches only the daemon of this computer. `afterglow ctl` has no `--ssh` option.
In a session pane, `afterglow ctl COMMAND` acts on the session that holds that pane, through `AFTERGLOW_SOCKET` and `AFTERGLOW_PANE_ID`.
It does this when `AFTERGLOW_SESSION` is set.
The pane IDs are those of the daemon, in `AFTERGLOW_PANE_ID` of each session pane. They stay the same when another window shows the session.

| Command | Pane without `--pane` |
| --- | --- |
| `afterglow ctl COMMAND` in a session pane | The pane of `AFTERGLOW_PANE_ID` |
| `afterglow ctl --session NAME COMMAND` | The pane that has the focus in the tab that shows. `--session` does not read `AFTERGLOW_PANE_ID`. |

In `ls`, a pane of a session has `window` 0 and an empty `context`.
Its `tab_id` is the place of its tab from 1, which changes when the tab moves.

| Problem | Message | Exit status |
| --- | --- | --- |
| `--session` without a name, or with a word that starts with `-` | `--session needs the name of a session` | 2 |
| A session name that is not valid | `--session takes the name of a session: ` and the rule for names | 2 |
| No command after the name | `no command after --session NAME` | 2 |
| `--session` more than once | `--session is given more than once` | 2 |
| No daemon runs | `no mux daemon runs, so there is no session 'NAME'` | 1 |
| No session has the name | `no session named 'NAME'` | 1 |

A usage error also writes the usage on standard error.

The control socket of a window that shows a session also lists the session panes, with the pane IDs of that window.
It acts on them as on the panes of other windows.
A pane ID in the daemon and a pane ID in a window can have the same value.

The daemon follows `Control.enabled` and `Control.allowReadText` of the window that showed the session last, else those of the Afterglow that started the session.
A window that answers a command follows its own settings too.

| Command | No window shows the session | A window shows the session |
| --- | --- | --- |
| `ls`, `send-text`, `send-key`, `get-text` (screen, scrollback and last output, also with `--ansi`, `--cursor` and `--wrap-markers`) | The daemon acts | The daemon acts |
| `colors`, `set-colors`, `set-user-var`, `signal-program` | The daemon acts on its terminal and its program of the pane | The daemon acts, and the window shows the colors with the next frame. The control socket of the window does not set the colors and the user variables of a session pane, or signal its program. The daemon has them. |
| `close` | The daemon closes the pane, and its program ends. When it is the last pane of the session, the session ends, whatever `Mux.onClose` says. | As without a window |
| `split` | The daemon starts the program. The pane goes beside its pane, on the side that the command names, and gets the focus. | The daemon starts the program, and the window places the pane in the same way |
| `new-tab` | The daemon starts the program. The pane gets a new tab after the tab of its pane, and that tab shows. | The daemon starts the program, and the window places the pane in the same way |
| `focus` | The daemon keeps the focus for the next window | The window focuses the pane |
| `focus --direction` | The daemon finds the pane beside the pane in the session's layout, and keeps the focus for the next window | The window finds the pane and focuses it |
| `move-pane`, `detach-tab` | The daemon moves the pane in its layout. A move to a new window makes a new session, as in a window (see [move panes and tabs](#move-panes-and-tabs)). No window shows the new session. A move to a pane of another session joins that session (see [move a pane to another session](#move-a-pane-to-another-session)). | The window moves the pane, as a drag in the window does. A move to a pane of another session joins that session, as without a window. |
| `get-text --selection`, `get-text --last-visited-output` | Refused with `only a window that shows session 'NAME' has its SCOPE`: only a window has a selection and an output that a jump visited | The window answers |
| `get-text --first-output-on-screen` | The daemon reads its own screen, which shows the newest rows | The window answers, from the rows that it shows |
| `set-title`, `set-tab-title`, `resize`, `scroll`, `set-font-size`, `set-opacity`, `window`, `signal`, `notify`, `action`, `new-window`, `open-directory`, `projects`, `save-project`, `open-project`, `scripts` | Refused with `no window shows session 'NAME', and only a window can do CALL` | The window answers |
| `overlay`, `screenshot` | Refused with `no window shows session 'NAME', and only a window can do CALL` | The window runs the overlay or takes the picture, and answers when it is done |
| `watch` | The daemon sends the events of the session | The daemon sends the events of the session |

In the refusal, `CALL` is the name of the [control call](CONTROL.md#control-calls) of the command, such as `setTitle`, `runAction` or `screenshot`.
`split` and `new-tab` take `--env` for a pane of a session, and no other [launch option](CONTROL.md#launch-options): the daemon runs the program.
The daemon finds the `neighbors` of `ls` and the pane of `focus --direction` from the splits of the session's layout.
It weighs distances as a window does, in the size of the pane that a window measured last, else in cells.
The `foreground` of `ls` is the process in the daemon's terminal of the pane.
While a pane decodes an image, `get-text`, `send-key`, `colors` and `set-colors` fail at once with `pane N decodes an image: try again`.
A window waits up to 5 seconds for its own reads (see [limits](#limits)).
The daemon checks the arguments in another order than a window does.
`screenshot` fails with `denied` before the daemon checks the pane ID.
A pane ID that is not a whole number from 0 to 4294967295 fails with `INVALID_ARG`, not `CALL_FAILED`.

The overlay of `afterglow ctl overlay` runs on the computer of the window, over the pane, as an overlay of the window's own pane does.
Its program and its `--cwd` are of that computer.
It belongs to the client that asked for it, and it closes when that client goes.
The standard input of `afterglow ctl overlay` goes to the window in parts.
`screenshot` needs `Control.allowReadText`, of the session and of the window.

`afterglow ctl watch` gets the [events](CONTROL.md#events) of the panes of the session from the daemon, also while no window shows it.

| Event | For a session |
| --- | --- |
| `paneCreated`, `paneClosed` | When a pane of the session starts or ends |
| `paneMoved` | When the window that shows the session moves a pane, as for a pane of its own. Also when `afterglow ctl move-pane` moves one while no window shows the session. |
| `focusChanged` | When another pane has the focus in the tab that shows |
| `titleChanged`, `statusChanged`, `bell`, `userVarChanged`, `commandStarted`, `commandFinished` | As in a window |
| `contextChanged` | Never. The daemon knows no context of a pane, and `ls` gives it as empty. |

A move to a new window (`afterglow ctl move-pane 0 new-window`) and `afterglow ctl detach-tab` end the pane for the watchers of the old session.
They get `paneClosed`. The new session has events of its own.
A move to another session ends the pane for them too, and they get `paneClosed`.
The watchers of the other session get `paneCreated`. The pane ID in the daemon stays the same.

The daemon merges the events as a window does.
It uses `Control.eventInterval` of the window that showed the session last, else of the Afterglow that started the session.
It sends the command of `commandStarted` and `commandFinished`, and the value of `userVarChanged`, only with `Control.allowReadText`.
When the session ends, the daemon sends `paneClosed` for its panes, then ends each subscription with the error `NO_SUCH_SESSION`.
When `Control.enabled` goes off, it ends each subscription with the error `NO_SUCH_TARGET`, as a window does.
It then sends no `paneClosed`, because the panes still run.
`afterglow ctl watch` then exits with status 1 and says why.

A client may end its side of the connection after its calls.
The daemon still sends the answers that windows give and the events of its subscriptions, then closes the connection.

A session of another host reaches only what its programs could reach over ssh.
No program of this computer runs for it, and it reads and writes nothing of this computer.
Its calls come from a client on that host, in a pane of the session, for example an `afterglow ctl` that you installed there.
`afterglow ctl --session` on this computer does not reach it.

| Call from a session of another host | The window |
| --- | --- |
| `overlay`, `screenshot`, `new-window` (with or without a command), `open-directory`, `open-project`, `save-project`, `projects`, `scripts` | Refuses it: `a session of another host runs no program on this computer` |
| `action script:ID.COMMAND` | Refuses it with `a session of another host runs only the script commands that a script marks remote, with aglow run`. `aglow run` in the session runs the script commands that a script marks remote. |
| `action` with `paste`, `copy`, `select_all`, `show_last_output`, `show_first_output_on_screen`, `show_last_visited_output`, `new_window`, `open_config`, `reload_config`, `use_package_shaders`, `save_project`, `switch_project`, `cancel_overlay` or `end_script_key_mode` | Refuses it with `a session of another host runs no program on this computer` |
| `notify` | Shows it. It counts against `Notifications.rateLimit` of the pane, as a notification of a program does. |
| `split` and `new-tab` with a `cwd` or an `env`, and the `split` and `new_tab` effects of scripts with them | Refuses them: `a pane of session NAME, which runs on another host, takes no cwd or env: it starts where the pane that it opens from is`. A new pane starts in the directory of the pane that it opens from, on the host. |

| Limit of the calls that a window answers | Value | At the limit |
| --- | --- | --- |
| Answer of the window | 24 MiB of JSON, which the window sends in parts of 1 MiB | The call fails with the code `failed`: `the answer is larger than 24 MiB` |
| Screenshots in progress | 4, for all the clients of the control socket and of sessions together | The call fails with the code `busy` |
| Inputs of `overlayInput` that wait for `overlay` | 4 for each client, 64 MiB for all the clients of sessions | The `overlayInput` call fails |

For `split` and `new-tab`, `afterglow ctl` prepares the program with your settings, as `afterglow mux new` does.
It reads the settings of the default config file, `$XDG_CONFIG_HOME/afterglow/config.json`, not the `--config` or `-o` of a running Afterglow.
It uses `Terminal.shell`, shell integration and `Terminal.env`, over the environment of the session.
`afterglow ctl` takes that environment from the daemon, so a split from an ssh login or cron starts the shell of the session.
`new-tab --cwd DIR` starts the new tab in `DIR`.
Without `--cwd`, and for `split`, the new pane starts where `Window.workingDirectory` says.
With `inherit`, the default, it starts in the directory of the program in the foreground of its pane.
`new-window` opens a window outside the session in the Afterglow of the window, and prints the ID of its pane there.
The results of `open-directory`, `open-project` and `save-project` also use the IDs of that Afterglow for their window and its panes.

### Script commands in a session pane

In a session pane, `aglow run` and `afterglow run` run the commands of the scripts of the window that shows the session.
The daemon runs no scripts. It passes the connection of the command to that window.
The window runs the command as for one of its own panes, with its own settings (`Control.enabled`, `Scripts.enabled` and the limits of `Scripts`).
A command acts on the pane that it names, as the window shows it. `aglow run` names the pane of the client, unless `--pane` names another pane of the session.
A command that names no pane (pane 0) acts on the focused pane of the window that shows the session.
The output, the exit status and the input of the command are as for a pane outside a session.

A session of another host (`--ssh`) gets what an [ssh session](CONTROL.md#remote-commands) of that host gets.
Its panes run only the commands that a script marks `remote`, and only with `Terminal.sshRemoteCommands` on in the window.
A command acts on the pane of the client, and `ev.source` is `remote`.
`ev.remote.host` is the host of the session, as `--ssh` names it.
`aglow run` with no arguments lists only the commands that are marked `remote`.
A command that is not marked `remote` fails as a command that does not exist.

| Case | Result |
| --- | --- |
| No window shows the session | `CALL_FAILED`: `no window shows session 'NAME', and only a window runs the commands of scripts` |
| The client does not run in a session pane of the daemon, nor below a program of one. A program that runs a terminal of its own, such as an editor, stays in the pane. A server that left the pane, such as a tmux server, does not. | `CALL_FAILED`: `only a client in a session pane reaches the mux daemon for the commands of scripts` |
| The session has 64 command clients already | `CALL_FAILED`: `session 'NAME' has 64 command clients already` |
| The window's ID of the pane makes the run request longer than 1 MiB. `aglow run` refuses such a run before it sends it. | `CALL_FAILED`: `with the window's ID of its pane, the run request is over 1 MiB` |
| The run names a pane of the daemon that the window does not show | `CALL_FAILED`: `no pane N of session 'NAME' shows in its window` |
| The window closes or detaches while the command runs | The command ends. The client gets `the window of session 'NAME' closed, and with it the runs of this client`, and exits with status 1. |

### Remote commands of an ssh session in a session pane

`afterglow ssh` in a session pane gets [remote commands](CONTROL.md#remote-commands) as in a pane outside a session.
The daemon keeps the socket that ssh forwards, because the ssh session runs in the daemon and outlives a window.
In a session of another host, the ssh session also runs on that host.
There, `afterglow ssh` needs `afterglow` and its shell integration on that host.
The build of [`aglow` and `aglow-mux` alone](BUILD.md#build-aglow-and-aglow-mux-alone) installs no `afterglow`.
The daemon passes each connection of the remote host to the window that shows the session.
The window runs the command as for an ssh session of its own.
The daemon also gives the pane its answer to the query of `aglow run` (OSC 24569).

| Item | Value |
| --- | --- |
| Local socket | `remote-ID-TOKEN.sock` with mode 0600, in the directory of the socket that session panes name in `AFTERGLOW_SOCKET`: `$XDG_RUNTIME_DIR/aglow-mux`, else `$XDG_STATE_HOME/aglow-mux` (see [sockets](#sockets)). `ID` is the ID of the second socket of the daemon. A daemon that starts removes those of its `ID` that a daemon left when it was killed. |
| Local socket path | 107 bytes. With an `ID` of 32 digits, the name has 61 bytes, so the directory can have at most 45 bytes. At the limit, `afterglow ssh` forwards no socket. The daemon answers `CALL_FAILED`: `the socket path PATH is too long`. |
| Settings | `Terminal.sshRemoteCommands`, `Control.enabled` and `Scripts.enabled` of the window that showed the session last, else those of the Afterglow that started the session. The same settings of the window that runs the command apply too. When a window turns remote commands off, the sockets of the session close. |
| End | When the `afterglow ssh` process ends, when its pane closes, and when the daemon ends. The daemon then cancels the forward with `ssh -O cancel`, with the `ssh` on its own `PATH` |
| Limits | 16 sessions for each pane, 256 for the daemon, and 64 connections for each socket, as in a window |

The daemon gives a session pane no overlay. An overlay over a session pane runs in the window, on the computer of the window.
Thus `afterglow ssh` in an overlay gets no socket of the daemon.
The daemon gives a socket only to an `afterglow ssh` that runs in the pane that it names, or below a program of that pane.
It finds them by their processes.
A process of another terminal that names the pane gets `CALL_FAILED`: `` only `afterglow ssh` in pane N opens a remote socket for it ``.
When no window shows the session, a client of the remote host gets `CALL_FAILED`: `no window shows session 'NAME', and only a window runs the commands of scripts`.

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
| Query of the socket of an ssh session (OSC 24569) | The daemon answers with the socket of the newest ssh session of the pane (see [remote commands of an ssh session in a session pane](#remote-commands-of-an-ssh-session-in-a-session-pane)). Without one, it answers that there is none (`error=off`). |
| Visibility report (mode 2033) | The window says whether you can see the pane. Until a window says it, the program hears that you cannot see the pane. This is also true after a takeover, and while no window shows the session. |
| Images | The daemon keeps the images, also while no window shows the session. It decodes PNG (Kitty graphics `f=100`) and iTerm2 images (PNG, APNG, JPEG, GIF, WebP, BMP, TIFF and netPBM). Each image decodes in a process of its own, which the decoder of the daemon (`aglow-mux --decode-images`) starts. It decodes PNG, JPEG and WebP with libpng, libjpeg-turbo and libwebp, which the build includes. It decodes GIF, BMP, TIFF and netPBM with readers of its own. It converts an image with an ICC color profile or a cICP color space to sRGB with Little CMS. Each process has limits on its memory, its CPU time and the system calls that it can make. A decoder process that crashes or takes too long ends, and the daemon and the other sessions go on. The images do not decode on a kernel without seccomp filters, or on a kernel older than Linux 5.3 (`pidfd_open`). They also do not decode on a processor other than x86-64, little-endian AArch64, RISC-V 64 and little-endian POWER64. The daemon then writes why on its standard error, which is the journal when it runs in `aglow-mux.service`. When the decoder cannot start, images do not decode for a minute, and the daemon writes why. The window shows the images that the daemon keeps. |

While no window shows the session, the daemon also answers these requests.

| Request of a program | Result |
| --- | --- |
| Queries that the terminal answers itself (DA1, DSR, color queries) | The daemon answers |
| Query of the notifications on screen (OSC 99 `p=alive`) | The daemon answers with an empty list, because no notification is on screen |
| Bell (BEL) and flash (OSC 555) | Each pane keeps one bell and one flash for the next window (see [requests for the next window](#requests-for-the-next-window)) |
| Notification (OSC 9, OSC 777, OSC 99) | Each pane keeps the newest ones for the next window, up to `Mux.keptNotifications` (see [requests for the next window](#requests-for-the-next-window)) |
| Clipboard write (OSC 52 and OSC 5522) | The daemon drops it, because a late write would replace what you copied since. An OSC 5522 write gets `EPERM` as its status. |
| Clipboard read (OSC 52 and OSC 5522) | Refused, as when you deny the request |
| File transfer (OSC 5113) | Refused with `EPERM`, as when you deny the request. A transfer with `q=2` asks for no status and gets no answer. |
| Drag and drop (OSC 72) | A program that asks which types the terminal supports gets the answer of a window. A drag offer (`t=o`) gets `EPERM`. No drop arrives, and no drag starts. |
| File download (OSC 1337 `File`) | Refused: no one can answer the question. The program goes on, and the data goes nowhere. |
| File upload (OSC 1337 `RequestUpload`) | No answer: no one can choose the files or refuse them. The program waits until you stop it. |
| Pointer shape (OSC 22), user variables (OSC 1337 `SetUserVar`), commands of shell integration (OSC 133) | The daemon keeps the last pointer shape, the user variables and the running command for the next window |

### Requests for the next window

While no window shows the session, no one sees its bells and notifications.
Each pane keeps them, and the next window that shows the session gets them.

| Request of a program | What the pane keeps |
| --- | --- |
| Bell (BEL) | One bell for any number of bells. The window rings it, as `Terminal.bell` sets. |
| Flash (OSC 555) | One flash for any number of flashes. The window flashes the pane. |
| Notification (OSC 9, OSC 777, OSC 99) | The newest notifications, up to `Mux.keptNotifications` (0 to 100, 8 by default). When a pane has more, the oldest goes. All of them together are at most 256 KiB in each pane, a quarter of the smallest `Mux.eventQueue`. A notification of more than 256 KiB keeps no icon. |
| OSC 99 notification with the ID of a kept one | It replaces the kept one, as on the screen |
| OSC 99 close (`p=close`) | It removes the kept notification with its ID |

The window gets the bell and the flash first, then the notifications in the order that the programs sent them.
It shows each notification also when the pane has the focus, because you did not see the pane when the program sent it.
`Notifications.enabled` applies as for the other notifications.
Each kept notification counts against `Notifications.rateLimit` of its pane when the window shows it. The window drops the notifications over the limit.
A session of another host keeps them in its own daemon.
Its window shows them under `Notifications.enabled` and `Notifications.rateLimit` of that window, as the other notifications of that host.
A window that goes before it got a bell, a flash or a notification leaves it for the next window.
`Mux.keptNotifications` comes from the settings of the window that shows the session, else of its last window, else of the Afterglow that started it.
A change applies at once to a session that a window shows.

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
| File download (OSC 1337 `File`) | The window asks first, as for its own panes, and the program waits for the answer. The data then streams to the window. When the queue of the pane is full, the program waits: no data is dropped. A window that goes ends the download. See [a download to a slow disk](#a-download-to-a-slow-disk). |
| File upload (OSC 1337 `RequestUpload`) | The window asks, as for its own panes. The daemon writes the answer to the program in one piece, as typed input. A request that does not fit the queue of the pane gets no answer. Only the answer of the window that the daemon asked last reaches the program. When the window goes before its answer reaches the program, the next window that shows the session asks again. It asks only while the job that asked still holds the terminal. A terminal reset keeps the question. |
| Pointer shape (OSC 22) and user variables (OSC 1337 `SetUserVar`) | The window takes them as from its own panes. A window that attaches gets the last values. |
| Commands of shell integration (OSC 133) | The window takes them as from its own panes, with the time that the daemon measured. A window that attaches while a command runs gets its start. |

### A download to a slow disk

The window writes the data of a download to the disk on a thread of its own.
When the disk is slower than the program, the data waits in a queue of the window, up to 8 MiB.
While that queue is full, the window reads no more events of the session.
The bells, notifications and other requests of all panes of the session wait.
The calls of `afterglow ctl --session`, the refreshes of hidden panes and `aglow run` in a pane of the session also wait.
The output of the panes still shows.
A pane that opens in the session meanwhile shows nothing, because the window learns of it from an event.
A pane or the session that ends meanwhile stays in the window.
The events go on when half of that queue is on the disk.
Meanwhile the daemon keeps the events up to `Mux.eventQueue` (see [requests that wait](#requests-that-wait)).
Past that limit, the program of the download waits.

### Requests that wait

The requests wait for the window in a queue of the session and in a queue of each pane.
`Mux.eventQueue` sets the size of each queue. A change applies when Afterglow next attaches to a session.
A request that finds the queue of its pane empty always fits, up to the limit of one request.
A request that does not fit gets the answer of a session that no window shows (see [what the daemon answers](#what-the-daemon-answers)).
Thus a notification and an OSC 52 clipboard write that do not fit are dropped.
These requests are exceptions:

- An answer to a query that does not fit is dropped, as a window drops one past its own limit.
- When commands of file transfer and drag and drop do not fit, the window ends the transfer or the drag, as for its own panes.
- A download always fits. When the queue of the pane is full, the program waits until the window took enough of the data.
- An upload request that does not fit gets no answer, and the program waits until you stop it.

These requests do not take more room when more come:

- Bells and flashes that wait count once.
- The pointer shape and each user variable wait with their last value only.
- The end of a command always goes when its start went.

When the window goes, the requests that still wait get the answer of a session that no window shows.
Bells, flashes and notifications that still wait go to the next window (see [requests for the next window](#requests-for-the-next-window)).
A request that the window got already gets no answer from the daemon.
A clipboard read that waits for your answer gets the denial when its window closes and the session keeps running.
When a session pane closes in the window, a file transfer that the window ran gets no more answers.
A pane can move to a new session or to another session while a request of its program waits for the window.
A bell, a flash and a notification then go to the window that shows the session of the pane now, else to the next window of that session.
Another request gets the answer of a session that no window shows.

## Limits

| Item | Limit | At the limit |
| --- | --- | --- |
| Session name | 1 to 64 characters: ASCII letters, digits, `_` and `-`. The first character is an ASCII letter, a digit or `_`. | `afterglow mux new` refuses the name with exit status 2 |
| Socket path | 107 bytes | The daemon and the client leave out a socket with a longer path. When no socket is left, they stop and name the path. |
| Open files | The hard limit of `RLIMIT_NOFILE`. The daemon raises its soft limit to the hard limit when it starts. | A new connection closes at once. A new session gets an error that says why its program cannot start. A PNG or iTerm2 image does not decode. |
| Request line | 1 MiB, with its newline | The daemon answers `MALFORMED_REQUEST` and closes the connection. A new session sends the environment of `afterglow` in its request. A very large environment makes `afterglow mux new` fail with `the request is over 1 MiB`. |
| Lines that do not parse, one after the other | 16 | Each line gets `MALFORMED_REQUEST`. The daemon closes the connection after its answer to the 17th line. |
| Waiting output to one client | 32 MiB | The daemon closes the connection to a client that does not read its answers |
| Environment, arguments and directory of a session | UTF-8 text | `afterglow mux new` changes each byte that is not UTF-8 to U+FFFD |
| Wait for a window to start its stream after the answer to its attach call | 5 seconds | The session is free for another window, and the window does not show it |
| Command of a pane that the daemon keeps for a project | 64 arguments of at most 4096 bytes | The pane saves as a shell |
| Titles and commands of the panes that the daemon keeps, together | 256 KiB in a session | The panes past it save as shells, with the titles of their programs |
| Input of a window that a program does not read | 32 MiB in the queue of the PTY of the pane. Then the daemon holds the input of the pane, up to 4 MiB for all the panes of the window. Then 32 MiB for each pane in the window. | While the daemon holds less than 4 MiB, the other panes still get their input. At 4 MiB, the daemon stops reading the input of the window. The window sends which panes you can see, and its reads of the screen and the history of a pane, apart from its input. Input that waits does not stop them. At 32 MiB for one pane, the window refuses more input for that pane and shows a notice, as for a pane of its own. Closing the window with `Mux.onClose` kill still ends the session. |
| Frames to a window | About 2 MiB of state in each frame, and at most 4 MiB, the largest frame of the stream | Larger state goes in more frames, so a slow window gets fewer frames. It never holds a queue of old ones. A pane that you cannot see gets frames without its screen. |
| History rows to a window | `Terminal.scrollback` of the window, in rows | The daemon sends no older rows |
| Wait for the screen and the history of a pane that you cannot see, or that shows again before its screen came, before a read of its text or a screenshot | 5 seconds | The read gets the text that the window has, which can be older. The window does not draw or take input while it waits. |
| Image file that the daemon decodes | The limits of a pane outside a session (see [COMPATIBILITY.md](COMPATIBILITY.md)): 320 MiB of Kitty data, and 96 MiB of iTerm2 base64 | The error of a pane outside a session: `EFBIG` for data in the command, `EBADF` for a file or shared memory. An iTerm2 image does not show. |
| Image that the daemon decodes | 10,000 pixels on a side, and 320 MiB of pixels | A Kitty command gets an `EBADPNG` error. An iTerm2 image does not show. |
| Time to decode an image | 30 seconds, and a CPU time of 5 seconds and 1 second more for each 16 MiB of pixels | The decoder process ends. The image does not show, as above. The daemon writes a line on its standard error, at most once a minute. |
| Memory of a decoder process | Its memory at its start, and 8 times the decoded pixels and 64 MiB more | The decode fails. The image does not show, as above. |
| Scans of a progressive JPEG | 1,000 | The image does not show, as above. |
| Panes of a session | 256 | A new tab or split fails with `session 'NAME' has 256 panes, the most that a session can have`. The window shows `Could not open a tab` or `Could not split the pane` with that message. `afterglow ctl` and scripts get it as the error. |
| Title of a tab or a pane that the daemon keeps | 1,024 bytes of UTF-8 | The window sends the start of a longer title, up to the end of a whole character. |
| Memory of the daemon for a window that does not read | For each pane: the frames that its channel takes (4 MiB), one frame more, and the requests in the queue of the pane (`Mux.eventQueue`). For the window: the queue of the session and the input that the daemon holds (4 MiB). The queue holds `Mux.eventQueue`, and past it one more item: a request of a program, a call of `afterglow ctl` or a part of a command connection. While that queue is full, a call of `afterglow ctl` that goes to the window fails with an error that asks to try again. | The daemon makes no newer frame of a pane until its waiting frame went. It never waits for a window. |
| Region scrolls in one frame | 256 | Every row from the top of the highest region that scrolled to the bottom of the lowest leaves its region. A selection or a search match in those rows goes. |
| Requests that wait for a window | `Mux.eventQueue` (1 to 64 MiB, 4 MiB by default) in the queue of the session, and as much in the queue of each pane. A request that finds the queue of its pane empty passes the limit. The queue of the session also takes one request of any size while it is below the limit. | The answer of a session that no window shows (see [requests that wait](#requests-that-wait)) |
| One request to a window | 129 MiB: the largest clipboard write that a terminal takes, and its types | The answer of a session that no window shows |
| Notifications that a pane keeps while no window shows the session | `Mux.keptNotifications` (0 to 100, 8 by default), and 256 KiB | The oldest goes. A notification of more than 256 KiB keeps no icon. |
| Watch events of a pane that wait for the daemon | 1,024 events or 4 MiB | A new title, status or value of a user variable replaces the last one of its kind that waits. Other new events are dropped. |
| Variables of `Mux.updateEnvironment` | 256 names | Afterglow uses the first 256 and reports the others as a problem of the configuration |
| First answer of the daemon of another host, through ssh, to an `afterglow mux` command | `Mux.signInTimeout` (5 to 600 seconds, 120 by default) | The command stops with an error |
| First answer of the daemon of another host, through ssh, to a window | 5 seconds. ssh asks for nothing (`BatchMode`). | The window does not open, and the launch fails with an error |

The daemon sets no limit on the number of sessions, or on the number of connections of `afterglow mux` and `afterglow ctl`.
Each session and each connection uses open files.
The commands of scripts, the remote commands and the subscriptions have limits of their own.
A session has at most 64 command clients (see [script commands in a session pane](#script-commands-in-a-session-pane)).
The remote commands have the limits in [remote commands of an ssh session in a session pane](#remote-commands-of-an-ssh-session-in-a-session-pane).
A connection has at most 256 subscriptions.
