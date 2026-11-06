<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# fzf picker

This Luau package picks a saved project or a file in an overlay.
An overlay runs a program over a pane and returns its result to the script.
When [fzf](https://github.com/junegunn/fzf) is installed, fzf runs in the overlay.
Without fzf, the package shows the select dialog.

Copy this complete directory into `~/.config/afterglow/scripts/fzf-picker/`.
When you use a custom configuration file, use the `scripts/` directory beside that file.
From the repository root, install it with:

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
| `file: PATH` | Types the quoted path at the prompt of the pane. |

The list has the saved projects, then the files below the directory of the pane.

| Setting in `config` | Default | Use |
| --- | --- | --- |
| `fzf` | `fzf` | The fzf program |
| `max_depth` | 4 | The depth of directories that the file search goes into |
| `max_files` | 5000 | The most files in the list |

The select dialog shows the first 500 entries.
