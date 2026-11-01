---
name: ag-add-setting
description: "Add or change an Afterglow configuration setting: the struct default, the schema KeyDef, the store read, the live apply, the test and the generated reference. Use when: new setting, new config key, make it configurable, change a default, rename a key, Settings.cpp, buildSchema, CONFIGURATION.md."
---

<!--
SPDX-FileCopyrightText: 2026 fuddlesworth
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Add a setting

A value that a user could want to change is a setting. The schema in `afterglow/src/config/Settings.cpp` is the
source of the defaults, ranges, choices and descriptions. `docs/CONFIGURATION.md` is generated from it.

`Animation.paneEffectDuration` is a complete example. Follow it with `grep -rn paneEffectDuration afterglow/`.

## Steps

1. **The field.** Add it with its default to the struct in `afterglow/src/config/Settings.h`, with a short `///<`
   comment. The default is in two places, here and in the schema. Keep them equal.
2. **The schema.** Add a `KeyDef` to its group in `buildSchema()` in `Settings.cpp`. Use `intKey` or `doubleKey`
   (with a minimum and maximum), `enumKey` (with its choices), `colorKey`, `stringKey` or the boolean helper. The
   store clamps and coerces from this definition, not from the struct.
3. **The description.** It is user-facing prose under `docs/WRITING.md`: one or two short sentences, the unit, and
   what an empty or zero value means. No em dash between clauses, no semicolons between sentences.
4. **The read.** Map the key into the struct in `SettingsStore::read()` (`st.read<T>(Group, QStringLiteral("key"))`).
5. **The live apply.** Settings reload while Afterglow runs. Apply a change where the value is used: compare with
   the previous settings in `TerminalWindow::applySettings`, the reload handler in `app/Application.cpp`, or
   `ControlBus::applySettings`. A setting that needs a restart says so in its description.
6. **The test.** Add the default and an override to `afterglow/tests/ConfigTest.cpp`. Test the clamp for a numeric
   key and the fallback for an enum.
7. **Build, then regenerate the reference:**

   ```sh
   cmake --build build -j6 && python3 tools/generate-reference.py build/afterglow/afterglow
   ```

   Never edit `docs/CONFIGURATION.md` by hand.
8. **The documents.** Mention the setting in the user document of its area (`docs/APPEARANCE.md`,
   `docs/SHADERS.md`, `docs/INPUT.md` and others), and in the `## Unreleased` list of `CHANGELOG.md` if it is a new
   feature.

## Renames and removals

Afterglow is not released. Rename or remove a key directly. Write no migration, no fallback read from the old key,
and no code that clears old keys. A config with the old key gets the default.

## Write through the store

Code that changes a setting for the user (an action such as Use Package Shaders) writes with
`SettingsStore::write(group, key, value)`, which commits and reloads. Never write `config.json` directly.
