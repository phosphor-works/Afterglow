---
name: ag-script-api
description: "Add or change a piece of the Afterglow Luau script API: an effect kind (afterglow.foo(...)), an event handler (on_*), a result field or a package manifest field. Covers the prelude, the type declarations, ScriptHost validation, ScriptControl routing, docs and tests. Use when: new script effect, script helper, Luau API, afterglow.luau, afterglow.d.luau, on_ event, ScriptHost, ScriptControl, package.json field."
---

<!--
SPDX-FileCopyrightText: 2026 fuddlesworth
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Extend the script API

A script returns effects, which are tables with a `kind`. `ScriptHost` checks them, and `ScriptControl` applies them
to windows through `Control`. `docs/SCRIPTING.md` and `docs/PACKAGES.md` are the contract for script authors.
Ergonomics matter: a script author should read the helper's name and arguments and know what it does.

`afterglow.pulse` is a complete example of an effect. Follow it with `grep -rn pulse afterglow/src/scripting/`.

## A new effect kind

1. **The helper.** Add `afterglow.foo(...)` in `afterglow/src/scripting/afterglow.luau`. Accept a pane object or an
   id (`paneId`). Give optional arguments defaults there, not in C++.
2. **The declaration.** Declare it in `afterglow/src/scripting/afterglow.d.luau`, and any new result fields in
   `OperationData`. The declarations and the prelude must agree.
3. **The kind table.** Add a row to `Kinds[]` in `ScriptHost.cpp`. `ScriptHost` rebuilds each effect from the fields
   that the row lists, so a field that is not listed is dropped with no message. Set `structural` if the effect
   creates or closes panes, tabs or windows (rate limits apply).
4. **Validation.** Add the checks for the kind in `applyEffect`: types, ranges and limits, with an error message
   that a script author can act on.
5. **Routing.** Route the kind in `ScriptControl.cpp`:
   - `c.apply` for an ordinary effect.
   - `c.applyOwned` for an effect that a script owns over time (a key mode, a deferred result). Release it in
     `clearScriptOwner`, and end it through the `failed` callback, so that a script error cannot trap the user.
   - An effect with no route fails at run time with "unknown effect".
6. **The behaviour.** Implement it on `Control` and `TerminalWindow` (`TerminalWindowScript.cpp`).
7. **Await.** An effect with a result works with `afterglow.await` automatically: the result must answer the
   request ID. Check one call with `afterglow.await` and one inside `afterglow.await_all`.

## A new event

Dispatch it from `ScriptControl.cpp` with the fields in snake case. Use `dispatchTo` for an event that only the
owner may see (key mode keys). Declare the handler and its event table in `afterglow.d.luau`.

## Documents and tests

- `docs/SCRIPTING.md`: the Helpers table, the section of the feature, and its limits with units.
- `CHANGELOG.md` under `## Unreleased`.
- `afterglow/tests/ScriptHostTest.cpp`: validation, the dropped-field trap, and await.
- `afterglow/tests/NativeWindowTest.cpp` when the window changes (`ag-verify-live`).
- An example in `data/examples/scripts/` or a package in `data/examples/packages/` for a feature that authors will
  copy. Examples use the await style.

## Package manifest fields

`ScriptPackage.cpp` parses `package.json`. Reject an unknown or malformed field with a clear diagnostic (it shows in
`afterglow ctl scripts`), test it in `afterglow/tests/ScriptPackageTest.cpp`, and document it in
`docs/PACKAGES.md`.
