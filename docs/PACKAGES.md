<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Luau packages and saved state

Afterglow supports single `.luau` files and local packages.
A package is a directory inside `scripts/` beside the active configuration file.
The directory contains a `package.json` manifest.
The directory can be a symlink, for example to a Git checkout.
Afterglow checks the package paths against the directory that the symlink points to.

## Manifest

```json
{
  "id": "my-extension",
  "version": "1.0.0",
  "api_version": 1,
  "state_version": 1,
  "entry": "main",
  "modules": {"main": "init.luau", "format": "format.luau"},
  "config": {"enabled": true},
  "dependencies": {"utils": {"path": "vendor/utils", "version": "1.0.0"}},
  "assets": {"theme": "assets/theme.json"}
}
```

The manifest ID must equal the directory name.
`version` uses the `X.Y.Z` format. `api_version` must be `1`.
Each module returns a table.
`require("format")` loads the declared module once for each instance.
`require("@utils")` loads the entry module of the included dependency.
The dependency must have its own manifest and the exact declared version.

Imports use local files. Afterglow does not download dependencies.
Dependencies share the caller's sandbox and state namespace.
Cycles and undeclared imports cause an error.

| Source limit | Value |
| --- | --- |
| Packages in one package tree | 16 |
| Modules | 64 |
| Each source file | 256 KiB |
| Each JSON file: `package.json`, `config.json`, a pack's `metadata.json` | 64 KiB |
| Total source, manifest and configuration data | 1 MiB |

A package over a limit does not load. `afterglow ctl scripts` shows the error.
An error in a module names the module file and its line.

## Configuration and assets

An optional `config.json` beside the manifest adds or replaces top-level `config` values.
`on_load(ev)` receives `ev.package` and `ev.storage`.

| Object | Contents |
| --- | --- |
| `ev.package` | ID, version, API version, user home directory (`home`), configuration and asset paths |
| `ev.storage` | Stored value, schema version and revision |

Assets can be themes, shader presets or images.
Limits are 32 local files, 4 MiB for each file and 16 MiB in total.
Asset declarations supply paths. They do not activate a theme or shader.
Paths and symlinks must remain inside the package root.
This applies to modules, dependencies, assets, `config.json`, the shader directory, its `shared` directory, the pack directories and their `metadata.json` files.
Afterglow does not check the stages and includes that a shader pack reads.

Afterglow monitors source files, dependency manifests and configuration together.
A change replaces the complete instance. This includes a change to `shaders` or `pulses`.
Compilation, initialization or migration errors keep the previous working instance.

## Saved state

```lua
local script = {}
function script.on_load(ev)
    local state = ev.storage.value
    state.launches = (state.launches or 0) + 1
    local _, err = afterglow.await(afterglow.state_write(state, ev.storage.revision))
    if err then return afterglow.log("State was not saved: " .. err.message) end
    return nil
end
return script
```

Wait for the results of `state_read()` and `state_write(value, revision)` with `afterglow.await`.
The result contains `value`, `version` and `revision`.
Storage uses the script ID below `state/scripts/`, beside the active configuration.
Writes use a lock, revision comparison and atomic file replacement.
A revision conflict returns `state_error`.
Read the current state before you merge changes and try again.

| State limit | Value |
| --- | --- |
| Encoded size of the value | 255 KiB |
| Nesting depth, the top-level object included | 16 |
| Values, tables and the top-level object included | 8192 |
| Each string | 65,536 characters |
| Each object key | 256 characters |
| Each number | Finite, magnitude at most 9,007,199,254,740,991 (2^53 − 1) |

A `state_write` over a limit fails with `state_error` and writes nothing.
The top-level value must be a JSON-compatible object table.
Nested lists are permitted.
Unsupported values, symlinks and corrupt files cause an error.
Afterglow keeps corrupt data for manual recovery.
Single-file scripts use schema version 1 and the same state effects.

## State migration

Increase `state_version` when the stored structure changes.
Export `migrate(value, oldVersion, newVersion)` to return the new object.
Afterglow calls `migrate` once, from the stored version to the current `state_version`.
Handle every older version in that one call.
Migration and `on_load` each use the 100 ms action deadline.

Afterglow validates migration output before activation.
It writes the new state only after the replacement compiles and initializes.
Exceptions, timeouts and revision conflicts keep the original file and active instance.
A transition to an older state schema is not permitted.

Return effects from `on_load`.
Afterglow applies these effects after successful activation.
`on_load` can wait for results. See [waiting for results](SCRIPTING.md#wait-for-results).

## Shaders

A package can ship shader packs with the script that uses them.

```json
{
  "id": "my-extension",
  "shaders": {"path": "shaders", "suggests": {"overview": "wall"}},
  "pulses": {"ripple": 4}
}
```

`shaders.path` names a directory inside the package. The default is `shaders`.
Each directory in it is a shader pack. It can contain 1 to 16 packs.
A pack directory name has 1 to 64 letters, digits, `_` or `-`.
The directory `shared` holds files that packs include. It is not a pack.
The name `none` is reserved.
A pack's `metadata.json` must declare the ID `<package>.<pack>`, such as `my-extension.wall`.
A symlinked pack must point inside the package, and passes the same checks.
Configuration refers to the pack by that ID only.
So a package cannot collide with another package or replace a bundled pack.

The packs are available while the package is loaded.
They reload when they change, and disappear when you remove or disable the package.
A configuration that names a missing pack shows a notice, logs a warning and skips the pack, as for any pack.

A package never changes the configuration file by itself.
`shaders.suggests` maps `Shaders` keys to the package's packs.
The keys are `background`, `text`, `cursor`, `pane`, `window`, `overview`, `tabTransition` and `paneEffects`.
The **Use Package Shaders…** command (`use_package_shaders`) lists the suggestions of loaded packages.
It writes the one you select to the configuration file. A chain key gains the pack at its end.

`pulses` names effect numbers from 4 to 255 that the package's packs draw.
`afterglow.pulse(pane, "ripple")` then plays effect 4.
A pulse name uses letters, digits, `_` and `-`. It cannot be only digits.
The names `portal`, `flash` and `shake` are reserved.
The [scripted overview](../data/examples/packages/scripted-overview/README.md) ships a key script and its own overview shader.

## Reload and removal

```sh
afterglow ctl scripts
```

This command lists active, disabled, failed, last-good and superseded instances.
It includes paths, errors and execution counters.
Correct the file to try loading it again.
A package that failed on its stored state loads again when you correct or remove `state/scripts/<id>.json`.
A disabled package loads again when its source or configuration changes.

Module memory survives a reload when the source does not change.
Replacement, removal and process exit remove that memory.
Only explicit disk state survives these operations.
Package removal cancels tasks and removes UI contributions.
The stored data remains available for a later installation.

## Example packages

| Package | Function |
| --- | --- |
| [Project navigator](../data/examples/packages/project-navigator/README.md) | Finds directories and opens project layouts |
| [Project autosave](../data/examples/packages/project-autosave/README.md) | Saves selected layouts periodically and supplies a restore selector |
| [Tab status](../data/examples/packages/tab-status/README.md) | Shows Git, directory, command and exit information |
| [Scene controls](../data/examples/packages/scene-controls/README.md) | Selects, changes and saves shader parameters |
| [Contextual scene](../data/examples/packages/contextual-scene/README.md) | Changes scene intensity from audio and history data |
| [Scripted overview](../data/examples/packages/scripted-overview/README.md) | Drives the pane overview from its own keys, with its own overview shader |

Copy a complete package directory into `~/.config/afterglow/scripts/` to install it.
Use the package instructions for configuration and actions.
