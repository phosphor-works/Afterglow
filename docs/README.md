<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# User documents

Afterglow is a Linux terminal with GPU rendering, tabs, splits, shell integration and Luau scripts.

| Task | Document |
| --- | --- |
| Build or install | [Build instructions](BUILD.md) |
| Set fonts, colors or themes | [Appearance](APPEARANCE.md) |
| Find a setting or default binding | [Configuration reference](CONFIGURATION.md) |
| Use keybindings, selection, search or hints | [Input](INPUT.md) |
| Move panes or save layouts | [Projects](PROJECTS.md) |
| Control a running instance | [Command line and D-Bus](CONTROL.md) |
| Write scripts | [Luau API](SCRIPTING.md) |
| Install or write packages | [Packages and state](PACKAGES.md) |
| Configure reactive effects | [Shaders](SHADERS.md) |
| Write shader passes | [Shader API](SHADER-API.md) |
| Use accessibility, input methods or files | [Desktop integration](DESKTOP.md) |
| Use bidirectional text | [Bidirectional text](BIDI.md) |
| Check protocol support | [Compatibility](COMPATIBILITY.md) |
| Upgrade | [Migration](MIGRATION.md), [release notes](../CHANGELOG.md) |
| Run tests | [Testing](TESTING.md) |
| Measure performance | [Performance](PERFORMANCE.md) |
| Write documentation | [Writing rules](WRITING.md) |

## First configuration

The usual configuration path is `~/.config/afterglow/config.json`.
`XDG_CONFIG_HOME` can change its parent directory.

```json
{
  "Font": {"size": 12, "antialiasing": "grayscale"},
  "Colors": {"theme": "afterglow-dark"},
  "Terminal": {"shellIntegration": true, "scrollback": 10000},
  "Window": {"padding": 8}
}
```

Afterglow reloads configuration changes automatically.
Shell integration starts automatically for supported Bash, Zsh and Fish versions.
The command palette contains prompt navigation, output, search and project actions.

Example files do not activate themselves.
Copy a complete package directory into `~/.config/afterglow/scripts/` to install it.
Keep the directory name, manifest ID and module filenames unchanged.
Follow the package's instructions for required settings.
Reopen the palette after reload to see new actions.

`afterglow --help` lists launch options.
`afterglow ctl --help` lists live control commands.
`afterglow --config-reference` prints the runtime schema without a window or user configuration.
