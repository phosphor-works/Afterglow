<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Desktop integration

## Accessible terminal text

Each visible pane in the current tab is a terminal text object under its window in Qt accessibility and Linux AT-SPI.
Screen readers can query viewport text, UTF-16 cursor and selection offsets, character geometry and text changes.
Text preserves grapheme clusters and soft wraps.
AT-SPI selection changes update the terminal selection.
Tab changes, splits and zoom update the exposed pane list. Scrolling updates the text view.
Afterglow does not expose background tabs or panes that zoom hides.
While the pane overview is open, Afterglow exposes every pane of the current tab, also a pane that zoom hides.
The geometry of each pane is then its place in the split layout, not its card.
Qt Quick controls expose tab activation and the history scrollbar.

### Pane actions

Each pane object has an action interface.
Its actions are the command palette actions that act on that pane or start from it.

| Action | Effect |
| --- | --- |
| `Show Menu` | Opens the pane's context menu at its cursor. This is the first action. |
| A palette action | Runs the action in that pane, as its keybinding does. |
| `Set Focus` | Asks for the keyboard focus for the pane and its window. The compositor can refuse the window activation. This is the last action. |

| Group | Pane actions |
| --- | --- |
| Text | Copy, Paste, Select All, Search, Keyboard Copy Mode, Select Text or File Reference, Open a URL on Screen, Copy a URL on Screen, Insert Unicode Character |
| Terminal | Clear Screen and Scrollback, Reset Terminal, Change Pane Title, Notify When the Next Command Finishes, View Last Command Output in Pager |
| Scroll | Scroll Page Up, Scroll Page Down, Scroll to Top, Scroll to Bottom, Jump to Previous Prompt, Jump to Next Prompt |
| Layout | Split Right, Split Down, Split Left, Split Up, Resize Pane Left, Resize Pane Right, Resize Pane Up, Resize Pane Down, Swap Pane Left, Swap Pane Right, Swap Pane Above, Swap Pane Below, Focus Pane Left, Focus Pane Right, Focus Pane Above, Focus Pane Below, Close Pane, Toggle Pane Zoom |
| Move and open | Move Pane to New Tab, Move Pane to New Window, New Tab, New Window |

New Tab and New Window start in the directory of the pane, as the `Window.workingDirectory` setting permits.

The other palette actions act the same from every pane.
They switch, move, detach and close tabs, close the window, change the font and switch to fullscreen.
They also equalize and rotate splits, and open the overview, shaders, projects, the configuration and the palette.
End Script Key Mode is not a pane action.
Use the command palette or the tab bar for these actions.
Script commands are not pane actions.
Run them from the command palette, the context menu or a key binding.

The name of a pane action is its title in the command palette, for example `Split Right`.
A screen reader says this name.
The key bindings of a pane action are the keys that the palette shows for it.
An action runs in the pane whose object you use, also when another pane has the focus.
Afterglow first shows the window and the tab of the pane, and gives the pane the focus.
Confirmations, such as the close confirmation, show as they do for a key.
When an action has nothing to do, its key goes on to the program, but the accessible action does nothing.
For example, Scroll Page Up does nothing in a full-screen program, and Toggle Pane Zoom does nothing in a tab with one pane.

Pane actions do nothing while a prompt, a menu, the palette, the search field or a keyboard mode has the keys.
Keyboard modes are the overview, copy mode, URL hints, Unicode input and a script's key mode.
Keys cannot run an action then, and an action from outside would replace the answer that the prompt waits for.

### Docked areas

Each [docked area](SCRIPTING.md#docked-areas) that shows is an object under its window.
This includes the areas of the panes that show in the current tab, while the pane overview is closed.
The geometry of a pane object does not include the areas of that pane.
An area with items is a toolbar. An area without items is read-only static text.
Its name is the text of the area, one line for each row.
Each run with an `item` that takes cells is a button in the area.
Adjacent runs on one row with the same `item` are one button.

| Button property | Value |
| --- | --- |
| Name | The text of the button cells, without leading and trailing spaces. When that text is empty, the `item`. |
| State | Not focusable. The keyboard stays with the panes. |
| Geometry | The cells of the button |
| Action | `Press` sends `on_dock_click` with `button` set to `left`, as a left click does. |

`Press` does nothing when a prompt, a menu or another part of the window controls covers the button, as for a click.
Each press counts toward the handler call limit of the script.
A mouse click does not count.

New text for the same items renames the buttons.
When items come, go or change their order, the area gets new buttons.
An area with `tab_bar` hides the tab bar.
Its script can show the tabs as buttons in the area, as the tab line package does.
When the area has no tab buttons, use the tab keybindings.

### Terminal text

The accessible document contains the viewport. Scroll to read older content.
Afterglow reports a cursor outside the viewport as absent.
Terminal output is read-only.
Accessibility cannot rewrite output or move the shell's editing cursor through a document offset.
Rectangular selections report their encompassing range.
Behavior depends on the desktop's AT-SPI service and screen reader.
The acceptance test checks public AT-SPI interfaces. It does not certify every screen reader.

When output scrolls the viewport, the text-change events remove the lines that left the top and insert only the new text.
A screen reader that speaks inserted text then reads the new output, not the whole viewport.

AT-SPI uses a separate D-Bus bus, the accessibility bus.
While accessibility is active, any client on that bus can read the viewport text, as with other toolkits.
Any client on it can also run the pane actions, for example Paste, Close Pane and New Window, and press docked-area buttons.
The access settings of the control socket (`Control.enabled`, `Control.allowReadText`) and of scripts do not apply to these clients.

## Input methods and directionality

Preedit text preserves grapheme clusters.
Committed text uses the normal terminal input path.
Focused control editors receive their own composition events.
They report their own surrounding text and cursor geometry.

Input methods can query the live cursor row, surrounding text, UTF-16 cursor offset and cursor rectangle.
These queries work while the viewport is scrolled into history.
Context is read-only. Afterglow rejects surrounding-text replacement requests.
Delete or Backspace cannot safely represent arbitrary replacement ranges in an application-controlled terminal.

Afterglow does not request a Latin language preference.
Predictive text and automatic capitalization remain disabled.
Context contains only displayed cells. Afterglow does not synthesize password contents.

`Terminal.bidi` enables optional bidirectional layout on eligible primary screens.
Selection, pointer targeting and accessible geometry follow visual cell order.
See [bidirectional text](BIDI.md) for choices and limits.

## Drop files into a pane

Drag local files or directories from a file manager onto a pane.
Afterglow focuses that pane and inserts absolute paths quoted for Bash, Zsh and Fish.
Paths use the normal bracketed-paste encoder. Afterglow does not append Enter.
Drops are disabled while a native terminal prompt owns keyboard input or the target is held after exit.

| Item | Limit | At the limit |
| --- | --- | --- |
| Local paths per drop | 256 | Afterglow rejects the entire drop and inserts nothing. |
| Quoted UTF-8 text per drop, including separators | 1 MiB | Afterglow rejects the entire drop and inserts nothing. |

These items also reject the entire drop:

- Remote URLs, and `file` URLs with a host other than `localhost`.
- Relative paths.
- Names with control characters (C0, DEL or C1) or format characters such as bidirectional overrides.

Quotes and backslashes in a name are escaped outside the single quotes, so Bash, Zsh and Fish read the same path.
A local path dropped into SSH remains a local path. The drop does not upload a file.
Use `aglow transfer`, `scp` or another transfer tool for uploads.

## Receive a file from a terminal application

OSC 1337 `File` and `MultipartFile` downloads open a native Save dialog.
Downloads use `inline=0`, which is also the default.
Choose a destination and select Save, or cancel the request.
Afterglow writes nothing before Save.
The interface supports iTerm2 `it2dl` and SSH transports that pass OSC sequences through.

From a source checkout, run the example client:

```sh
python3 data/examples/compatibility/download.py /path/to/report.txt
```

| Item | Limit | At the limit |
| --- | --- | --- |
| Decoded data of one download | 8 MiB | Afterglow ignores the request. |
| Pending requests in the application | 1 | Afterglow ignores other requests while the dialog is open. |

Invalid base64 and declared size mismatches reject the request.
The suggested name uses only the basename, without control or format characters.
Writes use `QSaveFile`. The native dialog controls overwrite confirmation.
Closing the source pane cancels its dialog.
The protocol has no success acknowledgement.
Afterglow does not open or execute received files automatically.

## The aglow client

`aglow` is a client of four Kitty protocols: file transfer, drag and drop, graphics and the clipboard.
This document describes each of them.
`aglow run` also runs the commands of scripts in the Afterglow on the same computer (see [script commands](CONTROL.md#script-commands)).
On a remote host, `aglow run` runs the commands that a script marks `remote`, through `afterglow ssh` (see [remote commands](CONTROL.md#remote-commands)).
`aglow` is one static binary with no shared libraries.
It runs on a Linux computer of its architecture whose kernel is as new as the static C library of the build host needs.
On x86-64, the processor must also have the instruction level that the build needs.
When `readelf` is installed, the build prints both requirements, for example "needs Linux 6.1.0 or later".
When the compiler or linker flags contain `-fsanitize`, `aglow` links dynamically. See [build and install](BUILD.md#install).
Afterglow installs it next to `afterglow`. To use the protocols over SSH, install `aglow` on the remote host (see [remote hosts](#remote-hosts)).
The `kitten` client of Kitty also works with Afterglow.
`aglow transfer`, `aglow dnd`, `aglow icat` and `aglow clipboard` take the options of the `kitten` command of the same name.
The differences are in [aglow interoperability](COMPATIBILITY.md#aglow-interoperability).

| Command | Function |
| --- | --- |
| `aglow transfer` | Copies files and directories to and from the computer that runs Afterglow |
| `aglow dnd` | Drags files and data out of a pane, and receives drops |
| `aglow icat` | Shows images in the pane |
| `aglow clipboard` | Reads and writes the clipboard. With OSC 5522, it can use any MIME type. With OSC 52, it uses plain text only. |
| `aglow run` | Runs a command of a script in the Afterglow on the same computer, or in the Afterglow that `afterglow ssh` came from |
| `aglow remote-prune DIR` | Removes the sockets of ended ssh sessions from `DIR`. The probe of `afterglow ssh` runs it on the remote host (see [remote commands](CONTROL.md#remote-commands)) |

`afterglow run`, `afterglow transfer`, `afterglow dnd`, `afterglow icat` and `afterglow clipboard` run the `aglow` next to `afterglow`.
When they cannot run it, they exit with one of these statuses, as a shell does:

| Status | Cause |
| --- | --- |
| 127 | There is no `aglow` next to `afterglow`, or the build has no `aglow` (`AFTERGLOW_BUILD_AGLOW=OFF`). `afterglow` also exits with 127 when it cannot read its own executable path from `/proc/self/exe`. |
| 126 | There is an `aglow` next to `afterglow`, but it cannot run. For example, it is not executable. |

`aglow --version` shows its version and the version of its protocol.
Run `aglow COMMAND --help` for the options of a command.
`aglow` with no command, or with a command that it does not know, exits with status 2.

| Example | Result |
| --- | --- |
| `aglow icat ./picture.png` | Shows an image. An animated GIF plays. |
| `aglow clipboard ./picture.png` | Copies a file to the clipboard as an image. |
| `aglow clipboard --get-clipboard --mime image/png ./copy.png` | Writes the image on the clipboard to another file. |

### Remote hosts

Afterglow does not copy `aglow` to a remote host.
Copy the static `aglow` of your build to a remote host whose kernel and processor it supports.
You can also install `aglow` on the remote host from a package, or build it there with `AFTERGLOW_AGLOW_ONLY` (see [build aglow alone](BUILD.md#build-aglow-alone)).
That build does not need Qt or the Phosphor libraries.
For file transfer, drag and drop, images and the clipboard, the remote host needs `aglow` or `kitten`, because these are Kitty protocols.
For script commands, the remote host needs `aglow`, because `kitten` has no counterpart of `aglow run`.
`aglow run` there runs the script commands that are marked `remote` when `Terminal.sshRemoteCommands` is on.
See [remote commands](CONTROL.md#remote-commands).

### aglow icat

`aglow icat` shows PNG, JPEG, GIF, BMP, TGA, HDR and PIC images.
It also shows PSD images in RGB, and binary PGM and PPM images. It does not show PBM images.
It sends a PNG image that needs no change as it is.
It decodes the other images to pixels. It also decodes a PNG image that it must scale or mirror, or draw on a color (`--background`).
It turns a JPEG image as its EXIF orientation tells.
A child process decodes each image, with limits on its memory and CPU time.
A corrupt image stops only that process, and `aglow icat` reports an error.
A GIF that is damaged after its first frame plays the frames before the damage, and `aglow icat` shows a warning.

For a directory, `aglow icat` shows the image files in it and in its subdirectories, in name order.
It finds them by the file name extension.
A subdirectory that it cannot read is left out with a message.
`aglow icat` then shows the other images, and exits with status 1.

When the terminal can read local files, `aglow icat` sends an image through shared memory or a temporary file.
Otherwise, it sends the image through the terminal connection, and it compresses pixels with zlib.
Before it exits, `aglow icat` removes the temporary files and shared memory objects that the terminal did not read.

| Item | Limit | At the limit |
| --- | --- | --- |
| Side of an image | 10,000 pixels | `aglow icat` refuses the image before it decodes it. `--scale-up` never makes a larger side. |
| Decoded pixels of one frame (width × height × 4 bytes) | 320 MiB | `aglow icat` refuses the image before it decodes it. |
| Decoded pixels of all frames of an animated GIF | 320 MiB | `aglow icat` shows the first frame only, with a warning. |
| Pixels of all frames at the size that they show (width × height × 4 bytes) | 320 MiB | `aglow icat` shows the first frame only, with a warning. |
| Frames of an animated GIF | 1,024 | `aglow icat` shows the first frame only, with a warning. |

Ctrl+C stops `aglow icat` between two parts of the output, and while a child process decodes an image.
SIGTERM and SIGHUP also stop it at these points.

### aglow clipboard

`aglow clipboard` uses the Kitty clipboard protocol (OSC 5522).
In a terminal without it, `aglow clipboard` uses OSC 52, which carries plain text only.
A read waits as long as the permission prompt of the terminal stays open. Press Ctrl+C to stop it.
`aglow clipboard` writes an output file as a shell redirection does.
It truncates an existing file, and the file keeps its owner, mode and links.

| Item | Limit | At the limit |
| --- | --- | --- |
| MIME types in one read or one write | 64 | `aglow clipboard` refuses the request with a message. |
| Aliases in one write | 64 | `aglow clipboard` refuses the request with a message. |
| Data of one write | 128 MiB | `aglow clipboard` refuses the input file with a message. |

## Kitty file transfer

Afterglow accepts the [Kitty file-transfer protocol](https://sw.kovidgoyal.net/kitty/file-transfer-protocol/) on OSC 5113.
A terminal program can send or receive files through its terminal connection, including an SSH connection.

Afterglow asks for permission before a transfer accesses files.
The dialog names the program and its tab or split, and lists the paths.
For a receive request, it lists the requested source paths. For a send request, it lists the destination paths that the program chose.
The dialog lists at most 100 paths. The question shows the first 10 of them and the number of the others. Its details show all of them.
**Yes** approves only the paths that the dialog lists. The dialog does not change while it is open.
The dialog shows some characters of a path as `\uXXXX`, or as `\UXXXXXXXX` above U+FFFF.
These are control characters, line separators, invisible format characters (such as a zero-width space) and backslashes.
When a link above a path leads elsewhere, a second line under the path shows where it resolves to. When the path of your home directory goes through a link, your home directory and the paths under it get no such line.
A requested directory ends with a slash: the program can read all the files in it.

| Request | What the dialog lists | What happens to the other paths |
| --- | --- | --- |
| Receive | The first 100 requested paths | The other requests get `EPERM`. |
| Send | The destinations of the files that wait, up to 100 | A file that came later, or that the dialog did not list, gets a dialog of its own. When you answer the dialog in the cooldown that follows a denial, the files that it did not list get `EPERM`. This includes the files that came while it was open. |

A send session starts at once, because the program names its destinations only after the start.
The dialog opens when Afterglow has handled every command that had reached the window when Afterglow held the first destination.
A command that the program wrote at about the same time can come after the dialog opens, also from the same write.
A program that names many files can get more than one dialog, if some of its files come after that.
A file waits, with its data, until you approve its destination. Nothing is written before that. The approved files of the session continue meanwhile.
A finish waits until no file waits.
If you deny the first dialog of a session, the session ends with `EPERM`.
If you deny a later dialog, its files get `EPERM`. The files that you approved before continue.
The files that came while the dialog was open get one more dialog, in the cooldown that follows the denial.
That dialog lists the first 100 files that wait. These can include files that came after the denial and before the dialog opened.
It is the last dialog for these files. When you answer it with **Yes** or **No**, the files that it does not list get `EPERM`.
The files that come while it is open also get `EPERM` when you answer it. The dialog tells you this, also when it lists all the files that wait.
If you deny that dialog, the cooldown of 10 seconds starts again.
Except for that last dialog, no dialog opens in a cooldown. If the dialog of a file would open in a cooldown, the file gets `EPERM`.
If you approved no file of its session before, the session ends with `EPERM`.
A file that came in a cooldown can still get a dialog after the cooldown, for example when the dialog of another session was open until then.
Afterglow writes an approved file only where the dialog showed:

- A directory that existed when the dialog opened must still be at the path where links led it then.
- A directory that did not exist must be a directory that the transfer makes, not a link. A link that a program made after the dialog opened leads nothing elsewhere.
- Afterglow writes the temporary file, replaces the destination and makes links through the checked directory.
- A receive reads the requested files at the path that the dialog showed.

If one of these checks fails, the file gets an error status (`EPERM`, `ELOOP` or `ENOTDIR`).
Each pane shows one permission dialog at a time. A new session gets `EBUSY` while a dialog is open.
A session that started before the dialog opened waits for its own dialog until the open one closes.
After you deny a dialog, or after a program ends its session while its dialog is open, new sessions in that pane get `EPERM` for 10 seconds.
A file of a session that you approved before also gets `EPERM` in that time, if that one more dialog does not list it.

Run these commands on the computer where the terminal program runs:

| Command | Result |
| --- | --- |
| `aglow transfer ./report.txt /absolute/destination/report.txt` | Sends a file to the computer that runs Afterglow. |
| `aglow transfer --direction=receive /absolute/source/report.txt ./report.txt` | Receives a file from the computer that runs Afterglow. |
| `aglow transfer --transmit-deltas ./report.txt /absolute/destination/report.txt` | Updates an existing destination with a delta transfer. |

With one source, a destination that ends with a slash is a directory, and the source goes into it.
Without the slash, `aglow transfer` copies the source to the destination path itself.

The protocol supports directories, regular files, symbolic links, hard links, permissions and modification times.
Afterglow keeps the read, write and execute bits of a mode. It drops the set-user-ID, set-group-ID and sticky bits.
Kitty keeps them.
The protocol also supports zlib compression, delta transfers, progress reports and cancellation.
Afterglow writes each incoming regular file to a temporary file in its destination directory.
It opens the temporary file at the first data of the file, so a program can name all of its files at the start.
After validation, Afterglow replaces the destination and reports success.
A file without a mode gets the mode that a new file gets under the umask that Afterglow had when the pane started its first transfer.
Cancellation removes incomplete temporary files. Completed files remain in place.
A directory that existed before the transfer keeps its mode and its modification time.
When Afterglow sends a directory, it leaves out devices, sockets and pipes, and entries that it cannot read.
Afterglow writes a failure after the program finished the session to the `afterglow.desktop` log.

Set `Terminal.fileTransferPassword` only when a shared secret must replace the permission dialog.
Give the same secret to `aglow transfer --permissions-bypass`.
An empty setting always requires permission.
A session that gives a password gets no dialog. A wrong password, or a password when the setting is empty, gets `EPERM`.
After a wrong password, no pane of Afterglow checks a password for 10 seconds, and the `afterglow.desktop` log has a warning.
Other users can see a password on the command line in the list of processes.
Give the option one of these values instead:

| Value | Source of the password |
| --- | --- |
| A path that starts with `.`, `/` or `~` | The file |
| `-` | Standard input, without echo on a terminal |
| A number below 256 | That file descriptor |

| Resource | Limit | At the limit |
| --- | --- | --- |
| Sessions per pane | 8 | The new session gets `EBUSY`. |
| Files per session | 16,384 | The file gets `EINVAL`. A receive listing gets `EFBIG`. |
| Open files of regular and delta transfers per pane | 128 | The file gets `EBUSY` until another file completes. An upload holds two from its first data: its temporary file and its directory. A delta upload holds these and its old file from its start. |
| Open files of the transfers of all panes | Half of the `RLIMIT_NOFILE` soft limit of Afterglow | The file gets `EBUSY`. A delta upload holds four: its temporary file, its directory, its old file and the stream of its signature. |
| Outgoing file streams per pane | 128 | The file gets `EBUSY`. |
| Decoded data per packet | 4,096 bytes | The file fails with an error status. |
| Command size | 16,384 bytes | Every transfer in the pane is canceled with `ENOMEM`. |
| Input queue, output queue | 16 MiB each | Every transfer in the pane is canceled with `ENOMEM`. |
| Path length, name length | 4,096 bytes, 255 bytes | The request is refused. |
| Receive listing depth | 128 directory levels | The deeper entry gets `EFBIG`. |
| Receive listing size | 8 MiB encoded, less when other output is queued | The session gets `EFBIG`. |
| Requested paths, and the commands of send files, waiting for permission per pane | 16 MiB | A receive session gets `ENOMEM`. A send file gets `ENOMEM`, and data for a waiting file ends its session with `ENOMEM`. |
| Delta buffers per pane | 64 MiB | The file gets `ENOMEM`. The signature of a delta that waits to be sent counts until it is sent. |
| One delta signature or one literal block of a delta | 16 MiB | The file fails with an error status. |
| Block size of a delta signature from the program | 1 MiB, as in Kitty | The file gets `EINVAL`. |
| Bytes that a delta upload or a compressed upload writes when the program gives no file size | `Terminal.fileTransferDeltaLimit`, 64 GiB | The file gets `EFBIG`. |

A limit never authorizes more filesystem access.
File I/O, compression and delta calculation run outside the GUI thread.
Afterglow makes the delta data for a receive transfer in small steps while it sends the data.
It writes no temporary copy of the delta.
Uploads and other messages in the pane continue during a large delta.
Afterglow sends its files one at a time, so the other files wait until the delta is complete.

Kitty 0.49.2 can omit empty files when it receives a transfer.
Afterglow sends the required end-of-data packet, including for empty files.
To receive empty files from Afterglow, use `aglow transfer`.

`--transmit-deltas` tells the terminal about the destination when `aglow transfer` receives.
The terminal learns which destination files exist, their sizes and the hashes of their blocks.
When a program sends a delta upload, Afterglow tells it the hashes of the blocks of the file that the upload replaces.
Afterglow makes no delta against a file of 4,096 bytes or less, and replaces such a file whole.
`aglow transfer` names all of its other files at the start. It names at most 32 delta uploads that are not finished, and names the next delta upload when one finishes.
A delta upload that it names later can get a dialog of its own.

`aglow transfer` does not trust the file list that a terminal sends when it receives files:

- Each top entry must be the path that you asked for, in normal mode and with `--mode=mirror`.
- With `--mode=mirror`, an absolute path stays the same path here. `~`, `~/PATH` and a relative path go under the local home directory.
- The path of each child is the path of its parent and its own name, 4,096 bytes or fewer.
- A regular file in the list must have a size. A file that gets more data than that size fails with `EFBIG`, and a file that gets less fails with `EIO`.
- It refuses data after the end of a compressed file.
- It refuses a list in which two entries have the same path, or a path is not absolute.
- It refuses mirror sources that overlap, and sources with the same name that go into one directory.
- It follows no symbolic link when it writes, also none that was there before.
- A directory that existed before keeps its mode and its modification time.
- A received mode keeps only its read, write and execute bits, less the local umask. The set-user-ID, set-group-ID and sticky bits go.
- A new directory has mode 0700 until the transfer ends.
- With `--confirm-paths`, it creates nothing before you answer.
- A transfer that ends early leaves no temporary file.

## Kitty drag and drop

Afterglow supports the [Kitty drag-and-drop protocol](https://sw.kovidgoyal.net/kitty/dnd-protocol/) on OSC 72.
Programs can accept desktop drops or supply data for a desktop drag.
The protocol supports MIME selection, copy and move actions, chunked data, thumbnails, remote files and directory handles.

Run a client, then drag data between its terminal pane and another program:

| Command | Result |
| --- | --- |
| `aglow dnd ./report.txt ./images/` | Offers files, and accepts dropped files in the current directory. |
| `aglow dnd --drag text/plain:./source.txt --drop text/plain:./destination.txt` | Offers text, and saves dropped text. |

`aglow dnd` refuses an empty argument.
`aglow dnd --drop MIME:PATH` writes the dropped data to a temporary file next to `PATH`.
It replaces `PATH` only when the drop succeeds, so a failed drop leaves the file as it was.
The new file keeps the mode and the group of the file that it replaces.
`aglow dnd` writes in place, as a shell redirection does, in these cases:

- `PATH` is a device, a FIFO or a link.
- The file has more than one name.
- The file has another owner.
- The file is in a directory that `aglow dnd` cannot write.
- The file has a group that `aglow dnd` cannot give to a new file, because the user is not a member of the group.

With `--confirm-drop-overwrite`, `aglow dnd` asks before a drop replaces existing files.
Press Enter to replace them, or Esc to cancel the drop. Pasted text is no answer.
The question waits for `--confirm-drop-timeout` seconds, 20 by default, from 1 to 25.
The time starts at the last message of the drop, so the time that `aglow dnd` takes to copy the files counts too.
Then `aglow dnd` cancels the drop with a message.
Afterglow cancels a drop after 30 seconds without a message, so the limit is below that.

| Exit | Status of `aglow dnd` |
| --- | --- |
| Esc or Ctrl+C | 0 |
| A signal N, such as SIGTERM | 128 + N |
| An error, or the terminal closed | 1 |

A program must enable the protocol before it receives drag events.
A source offer requires a physical left-button drag gesture.
When a drag ends, Afterglow tells the source the action that the drop took (the `o` key of `t=e:x=4`).

| `o` value | Action of the drop |
| --- | --- |
| 0 | None. The drag was canceled, or the target refused it. |
| 1 | Copy |
| 2 | Move |

A move to another application is reported as a copy, both in `o` and in the action of `t=e:x=2`.
The source then keeps its files.
The reason is that KWin reports a drop that the other application refused in the same way as a drop that it took.
A move to a pane of Afterglow is reported as a move.

Kitty does not send the `o` key. Clients that follow Kitty ignore it.
`aglow dnd` deletes the files of a remote move only when the drop ended as a move.
It keeps a file that changed after `aglow dnd` sent its data, and shows a message.

When `aglow dnd` receives files from another computer, it reads them through the terminal.

| Item | Limit | At the limit |
| --- | --- | --- |
| Directories that `aglow dnd` keeps open in one drop from another computer | 192 | A directory past the limit comes without its entries. `aglow dnd` reports that some dropped entries could not be read. The drop is a copy, not a move. When the source permits only a move, `aglow dnd` reports no action, and the source keeps its files. The files that arrived stay in the drop directory. |

Afterglow rejects file reads before a drop occurs.
It also rejects file reads for a drag that starts in any Afterglow window.
A read of a file in the temporary directory of a remote drag is the exception.
Afterglow sends a desktop MIME type name to the program only when the name obeys these rules:

- It contains a `/`.
- It has 255 bytes or fewer.
- It contains only printable ASCII characters. A space is not permitted.

Programs without protocol support retain the quoted-path paste behavior for local file drops.

The desktop gets the program's answer to a drag only when the pointer enters the pane or moves.
When a drag shows new MIME types to the program, Afterglow waits for the answer.
The `Terminal.dragAnswerWait` setting gives the longest wait in milliseconds. The default is 100.
The window does not draw while it waits.
If the answer comes later, the drop target accepts the drag after the next pointer motion.

A drag source is local only when its machine id matches this computer.
Afterglow copies the files of every other source, including a source without a machine id, into a temporary directory.
The drop target receives paths in that directory, not the paths that the program named.
A remote source can also name `http`, `https`, `ftp` and `mailto` URLs. Other schemes reject the drag with `EPERM`.
Afterglow removes other file-list types, such as `application/x-kde4-urilist`, from a remote drag.
Drop targets often copy files after the drag ends.
After a drop, the directory remains for `Terminal.dragTreeLifetime` minutes. The default is 10.
A later drag and closing the pane do not remove it.
Afterglow removes the directory at once when the drag is canceled or fails.
Relative symbolic links must remain inside that directory.
Afterglow rejects absolute links and links that escape the temporary directory.
A `..` component is accepted only after a real directory, not after a link.
A dropped directory that a program reads through the protocol leaves out names that are not UTF-8.

| Resource | Limit |
| --- | --- |
| MIME types per offer | 256 |
| Data per MIME type | 64 MiB |
| Cached MIME data per drag | 256 MiB |
| Queued read data per drop | 256 MiB |
| Remote source data per drag | 1 GiB |
| Remote source entries | 16,384 |
| URIs in a remote drag | 1,024 |
| Remote path length | 4,096 characters |
| Remote symbolic link target | 4,096 bytes |
| Entries in one dropped directory listing | 16,384 |
| Directory entries listed per drop | 65,536 |
| Open directory handles of one drop, until the program closes them | 256 |
| Queued read requests | 64 |
| Thumbnail count | 16 |
| Decoded thumbnail data per drag | 64 MiB |
| Thumbnail dimensions | 4,096 × 4,096 pixels |
| Payload per packet | 4,096 bytes |
| Command size | 8,192 bytes |
| Chunked control message, such as a MIME list | 128 KiB |
| Machine id | 128 bytes |

An error in one read of a drop fails only that read. The program can read the other entries.
These errors fail one read:

| Error | Cause |
| --- | --- |
| `ENOENT` | The file or the entry does not exist. |
| `EPERM` | Afterglow cannot open the file, or the read is not permitted. |
| `EIO` | A read of the file or of its directory failed. |
| `EINVAL` | The request names no MIME type, entry or directory handle of the drop, or the file is not a regular file, a directory or a link. |
| `EUNKNOWN` | The URI is not a local file. |
| `EMFILE` | 64 read requests are already queued. |
| `ENOMEM` | The data of a MIME type is larger than 64 MiB, or a directory exceeds a listing limit or the limit of open directory handles. |

These errors end the drop:

- Queued read data of more than 256 MiB (`ENOMEM`).
- A packet or a chunked message over its limit (`EFBIG`), and a machine id longer than 128 bytes.
- Lost packets, and a reset of the protocol.
- No message from the program for 30 seconds.

A packet or a chunked message over its limit gets `EFBIG`. Afterglow drops the remaining chunks of that message.
A command over 8,192 bytes also ends the protocol. The program must enable it again.
An oversized packet or command gets no reply when the program accepts no drops, offers no drags and sends no chunked message.
Thus Afterglow writes no errors to a program that prints a binary file.
A stalled remote source preparation expires after 120 seconds.

## Native acceptance coverage

[Native window tests](TESTING.md#native-window-tests) gives the commands that build and run the suite.
The runner sends pointer events to the nested compositor's X11 backend.
The compositor produces Wayland pointer events and negotiates a real Qt `QDrag`.
The test filename contains Unicode and an apostrophe.
The test checks the quoted path received by the PTY.

The suite also checks these behaviors:

- Composition, commit and focused-editor input-method queries.
- Accessible controls and the focused chrome editor.
- Accessible pane actions, the pane menu and docked-area buttons.
- Bidirectional pointer selection.
- Pane launch failures, launch options and close identity.
- Paste confirmation ownership and keyboard acceptance.
- Button key releases.
- Notifications and script text permissions.
- Kitty drag and drop with `aglow dnd`: a text drag between two panes, and a remote directory drag. The same tests run with `kitten dnd` when it is installed.
- Overlays: standard input that a control client sends in parts, and the OSC 24569 answer to the program of an overlay.
- Pane and tab ends: the panes of the last tab close before their window, and the end of a pane alone in its tab is announced once. A window can be deleted while it shows.
- Terminal events that reach the GUI thread in their order, with a bound on the events that wait.
- Focus and keys: the palette closes when the focus moves, and a held key that closes a pane does not repeat into the next pane.
- A split whose shell does not start keeps the zoom, and the pointer row of a view that is scrolled by part of a row.

Composition tests use Qt events through the window, renderer and PTY.
They do not certify external IBus or Fcitx engines.
The separate `desktop_acceptance.py` suite checks AT-SPI and native file-save dialogs.
