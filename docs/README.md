<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# User documents

This index lists the user documents of Afterglow, a Linux terminal with GPU rendering, tabs, splits, shell
integration and Luau scripts. Users, script and shader pack authors, and developers read it to find the page for a
task. The [release notes](../CHANGELOG.md) tell what Afterglow supports.

## Pages

| Page | Description |
| --- | --- |
| [Appearance](APPEARANCE.md) | Sets fonts, colors and themes, and describes the [configuration file](APPEARANCE.md#configuration-file) `~/.config/phosphor-works/afterglow/config.json`, which Afterglow reloads when it changes. |
| [Bidirectional text](BIDI.md) | Tells how Afterglow shows bidirectional text. |
| [Build instructions](BUILD.md) | Tells how to build and install Afterglow. |
| [Compatibility](COMPATIBILITY.md) | Lists the terminal protocols that Afterglow supports. |
| [Configuration reference](CONFIGURATION.md) | Lists each setting and default binding, and `afterglow --config-reference` prints the same runtime schema with no window or user configuration when you give it as the only option. |
| [Command line and control socket](CONTROL.md) | Tells how to control a running instance (`afterglow --help` and `afterglow ctl --help`), with [script commands](CONTROL.md#script-commands) through `aglow run`, [remote commands](CONTROL.md#remote-commands) from an `afterglow ssh` session, [`afterglow ctl overlay`](CONTROL.md#overlays) and [screenshots](CONTROL.md#screenshots). |
| [Desktop integration](DESKTOP.md) | Covers accessibility, input methods, file and text drops, Kitty file transfer, drag and drop, and the `aglow` client. |
| [Input](INPUT.md) | Covers keybindings, the mouse, touchpads, touchscreens, selection, search, hints and shell integration, which starts automatically for supported Bash, Zsh and Fish versions. |
| [The MCP server](MCP.md) | Tells how to give an agent such as Claude Code a terminal session of its own, and `afterglow mcp --help` lists the options of the server. |
| [The mux daemon](MUX.md) | Tells how to keep terminal sessions running and show them in windows, and `afterglow mux --help` lists the commands that create, list and end sessions. |
| [Luau packages and saved state](PACKAGES.md) | Tells how to install and write packages. |
| [Native terminal performance](PERFORMANCE.md) | Gives the performance measurements and their test conditions. |
| [Panes and projects](PROJECTS.md) | Tells how to move panes, save layouts and open projects. |
| [Luau scripting](SCRIPTING.md) | Gives the Luau API for scripts, with [overlays](SCRIPTING.md#overlays) that run a program such as fzf over a pane or in a popup. |
| [Shader pass API](SHADER-API.md) | Gives the contract for authors who write shader passes. |
| [Shaders](SHADERS.md) | Tells how to configure reactive shader effects. |
| [Testing](TESTING.md) | Tells how to run the tests. |
