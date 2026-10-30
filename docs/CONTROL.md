<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# External control

Other programs can control Afterglow with the command line or D-Bus.
They can list panes, send text, create panes, set titles, show notifications and run actions.
The `watch` command supplies events as JSON lines.

```sh
afterglow ctl --help
```

The D-Bus interface is `io.github.fuddlesworth.Afterglow.Control`.
Its object path is `/Afterglow/Control`.

## Access settings

Set `Control.allowReadText=true` to permit external terminal text queries.
Set `Control.enabled=false` to disable the control interface.
These settings are separate from [script text permissions](SCRIPTING.md#text-and-semantic-regions).

## References

- [Pane movement and project commands](PROJECTS.md)
- [Shader signals and parameters](SHADERS.md)
- [Luau scripting](SCRIPTING.md)
- [All configuration keys](CONFIGURATION.md)

`afterglow --help` lists application launch options.
`afterglow --config-reference` prints the runtime configuration schema.
This command does not open a window or read user configuration.
