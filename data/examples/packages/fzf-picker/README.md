<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# fzf picker

This Luau package picks a saved project or a file in an overlay.
An overlay runs a program over a pane and returns its result to the script.
When [fzf](https://github.com/junegunn/fzf) is installed, fzf runs in the overlay.
Without fzf, the package shows the select dialog.

Install it in the `scripts/` directory beside the configuration file. For the default configuration, run this from the repository root:

```bash
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/fzf-picker ~/.config/afterglow/scripts/
```

Open the command palette and choose **Pick a project or file**.
This is the command `pick`. Bind it as `script:fzf-picker.pick`.
From a shell in a pane, run `aglow run fzf-picker pick`. The overlay opens over that pane.

| Choice | Result |
| --- | --- |
| `project: NAME` | Opens the saved project `NAME`. |
| `file: PATH` | From the palette or a key: types the quoted absolute path at the prompt of the pane. From `aglow run`: prints the absolute path on standard output. |

The package types the path only when the pane shows its prompt.
When a program runs in the pane, for example an editor, the package shows a notification instead.
This check uses [shell integration](../../../docs/INPUT.md#shell-integration). Without it, the package always types the path.

Because `aglow run` prints the path, you can use the result in a command:

```bash
vim "$(aglow run fzf-picker pick)"
```

The list has the saved projects, then the files below the directory of the run.
For `aglow run`, this is the directory of the shell. For the palette or a key, it is the directory of the pane.
The file search does not go into `.git` directories.
The list does not show a name with a control character, such as a line break.
It also does not show a name that is not valid UTF-8, because the script cannot type that name correctly.

Create `config.json` beside `package.json` to override the defaults:

```json
{
  "fzf": "/usr/bin/fzf",
  "max_depth": 6,
  "max_files": 10000
}
```

| Key | Default | Use |
| --- | --- | --- |
| `fzf` | `fzf` | The fzf program |
| `max_depth` | 4 | The depth of directories that the file search goes into |
| `max_files` | 5000 | The most files in the list. Saved projects do not count toward this limit. |

The select dialog shows the first 500 entries.
When the overlay fails for a reason other than a cancel, a notification tells why. For example, the pane can have an overlay already.
