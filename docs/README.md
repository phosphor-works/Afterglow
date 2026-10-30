<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Afterglow user reference

Afterglow is a Linux terminal with GPU rendering, tabs and splits, automatic shell integration and Luau extensions. Configuration reloads from `$XDG_CONFIG_HOME/afterglow/config.json` (normally `~/.config/afterglow/config.json`). Start with the examples below; files in the examples directory do not activate themselves.

| Task | Reference and working example |
| --- | --- |
| Configure fonts, colors, windows, input or audio | [Complete settings reference](CONFIGURATION.md), [font rendering](../README.md#font-rendering), [themes](../README.md#themes) |
| Navigate commands and copy output | [Shell integration](../README.md#shell-integration), [history, search and hints](../README.md#history-search-and-hints) |
| Change keyboard shortcuts and key tables | [Keybindings](../README.md#keybinds); all default bindings are in the generated settings reference |
| Move panes or restore a project | [Pane movement](../README.md#moving-panes-and-tabs), [saved projects](../README.md#named-projects), [project examples](../data/examples/projects) |
| Write an extension | [Luau API guide and index](SCRIPTING.md), [typed declarations](../afterglow/src/scripting/afterglow.d.luau), [packages](../data/examples/packages) |
| Build a visual scene | [Shader guide](../README.md#shaders), [live parameters](../README.md#live-named-shader-parameters), [Phosphor demo](../data/examples/phosphor/README.md), [measured presets](../data/examples/phosphor/VALIDATION.md) |
| Use accessibility, input methods or file workflows | [Desktop integration](DESKTOP.md) |
| Check protocol support and limits | [Compatibility](COMPATIBILITY.md), [interactive examples](../data/examples/compatibility) |
| Upgrade configuration or scripts | [Migration notes](MIGRATION.md), [release notes](../CHANGELOG.md) |

`afterglow --help` lists launch options; `afterglow ctl --help` lists live control commands. `afterglow --config-reference` prints defaults, descriptions, ranges and choices directly from the runtime configuration schema without starting a desktop window. It reads no user configuration.

For a minimal first customization:

```json
{
  "Font": {"size": 12, "antialiasing": "grayscale"},
  "Colors": {"theme": "afterglow-dark"},
  "Terminal": {"shellIntegration": true, "scrollback": 10000},
  "Window": {"padding": 8}
}
```

Shell integration runs automatically for Bash, Zsh and Fish. Run a command, then use the command palette to find prompt navigation, command output, search and project actions. Install a package by copying its whole directory under `~/.config/afterglow/scripts/`; its README explains the actions and required options. The package directory, manifest ID and module filenames matter. Open the palette again after reload to find the package's actions.
