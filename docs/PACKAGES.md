<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Luau packages and saved state

Afterglow supports single `.luau` files and local packages.
A package is a directory inside `scripts/` beside the active configuration file.
The directory contains a `package.json` manifest.

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
| Total source, manifest and configuration data | 1 MiB |

## Configuration and assets

An optional `config.json` beside the manifest replaces its corresponding `config` values.
`on_load(ev)` receives `ev.package` and `ev.storage`.

| Object | Contents |
| --- | --- |
| `ev.package` | ID, version, API version, home path, configuration and asset paths |
| `ev.storage` | Stored value, schema version and revision |

Assets can be themes, shader presets or images.
Limits are 32 local files, 4 MiB for each file and 16 MiB in total.
Asset declarations supply paths. They do not activate a theme or shader.
Paths and symlinks must remain inside the package root.

Afterglow monitors source files, dependency manifests and configuration together.
A change replaces the complete instance.
Compilation, initialization or migration errors keep the previous working instance.

## Saved state

```lua
local script = {}
function script.on_load(ev)
    local state = ev.storage.value
    state.launches = (state.launches or 0) + 1
    return afterglow.request("saved", afterglow.state_write(state, ev.storage.revision))
end
return script
```

`state_read()` and `state_write(value, revision)` return results through `afterglow.request`.
The result contains `value`, `version` and `revision`.
Storage uses the script ID below `state/scripts/`, beside the active configuration.
Writes use a lock, revision comparison and atomic file replacement.
A revision conflict returns `state_error`.
Read the current state before you merge changes and try again.

| State limit | Value |
| --- | --- |
| Encoded size | 256 KiB |
| Nesting depth | 16 |
| Values | 8192 |

The top-level value must be a JSON-compatible object table.
Nested lists are permitted.
Nonfinite numbers, unsupported values, symlinks and corrupt files cause an error.
Afterglow keeps corrupt data for manual recovery.
Single-file scripts use schema version 1 and the same state effects.

## State migration

Increase `state_version` when the stored structure changes.
Export `migrate(value, oldVersion, newVersion)` to return the new object.
Migration and `on_load` each use the 100 ms action deadline.

Afterglow validates migration output before activation.
It writes the new state only after the replacement compiles and initializes.
Exceptions, timeouts and revision conflicts keep the original file and active instance.
A transition to an older state schema is not permitted.

Return effects from `on_load`.
Afterglow applies these effects after successful activation.
Operation errors return through `on_result`.

## Reload and removal

```sh
afterglow ctl scripts
```

This command lists active, disabled, failed and last-good instances.
It includes paths, errors and execution counters.
Correct the file to try loading it again.

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

Copy a complete package directory into `~/.config/afterglow/scripts/` to install it.
Use the package instructions for configuration and actions.
