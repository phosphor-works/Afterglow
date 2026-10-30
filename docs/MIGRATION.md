<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Migration

## Desktop and protocol behavior

Configuration and Luau API versions do not change.
Existing settings, scripts and package state retain their formats.
`--config-reference` is read-only.

| Feature | Changed behavior |
| --- | --- |
| OSC 1337 downloads | Files without `inline=1` now open a Save dialog. Inline images retain their behavior. |
| File drops | Quoted paths enter the pane under the pointer. No newline is added. |
| Input methods | IMEs receive cursor-row context without a Latin preference hint. Surrounding-text replacement remains unavailable. |
| Relative images | Invalid parents and cycles return protocol errors. Applications that ignore replies should use `q=2`. |
| Extra cursors | Query support with `CSI > SPACE q`. Use the returned operation list. |

## Text, scripting and shaders

`Terminal.bidi` defaults to `off`.
Select `auto`, `ltr` or `rtl` for primary-screen paragraph ordering.
Copy and script queries retain logical text.
Application grids follow the [bidirectional layout policy](BIDI.md).

`Scripts.allowReadText` defaults to `true`.
Set it to `false` to deny terminal text, selection and semantic-region reads.
Existing scripts retain text access unless you disable this setting.

Launch options apply to tabs, windows and all split directions.
Failed executable launches return an error without retaining an empty split.
Environment values of `false` remove inherited variables.
See [scripting](SCRIPTING.md) for signatures and examples.

Shader packs support multiple buffers, image parameters, auxiliary depth and custom vertex hooks.
Existing one-buffer packs retain their behavior.
Live images change at transition completion. Reset restores the configured image.
Memory limits reject a complete live update and preserve current values.

## Configuration and package formats

`Terminal.copyOnSelect` accepts `primary`, `clipboard` and `off`.
Legacy booleans map to `primary` and `off`. Use named values in new configuration files.

Luau API version 1 uses stable pane, tab and window IDs.
It returns effects and correlated operation results.
Call `afterglow.request(id, effect)` and handle `on_result` for operations with results.
Creating an effect does not mean that it has completed.
Get shader instance IDs from `afterglow.shaders`. Instance IDs differ from pack IDs.
Reset live overrides with an empty parameter table.

Package manifests declare `api_version`, `state_version`, an entry module and a module map.
A stored state version change requires `migrate(value, oldVersion, newVersion)`.
State writes check revisions.
See [packages](PACKAGES.md) and the [typed API](../afterglow/src/scripting/afterglow.d.luau).
