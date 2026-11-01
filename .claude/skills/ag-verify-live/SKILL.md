---
name: ag-verify-live
description: "Verify an Afterglow window, pane, tab, input, overview or animation change in a real compositor, not only in unit tests. Runs the native window tests in a headless nested KWin, or the full acceptance runner with real compositor input. Use when: does it actually work, verify the fix, native tests, NativeWindow, test it live, reproduce the bug, before claiming a UI fix works."
---

<!--
SPDX-FileCopyrightText: 2026 fuddlesworth
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Verify in a real compositor

`afterglow_native_tests` drives real `TerminalWindow`s through a Wayland compositor: panes, splits, tabs, the
overview, tab transitions, script key modes, input methods and accessibility. Run it after a change to
`afterglow/src/TerminalWindow*`, `Pane*`, `SplitTree*`, `chrome/`, `desktop/` or the scripting glue that drives
windows. Unit tests do not cover these paths.

Never test against the user's desktop session. The user runs Afterglow as their terminal, and a test that types,
closes windows or kills processes there damages their work.

## Headless run (the default)

1. Build the test binary:

   ```sh
   cmake --build build -j6 --target afterglow_native_tests
   ```

2. Run it in a headless nested KWin. The runner starts itself under `dbus-run-session`, so KWin and the tests use a
   private D-Bus:

   ```sh
   python3 .claude/skills/ag-verify-live/headless_native.py > <scratchpad>/native.log 2>&1; echo $?
   grep -E '^\[  (FAILED|PASSED) |^\[==' <scratchpad>/native.log
   ```

The default filter is `NativeWindow.*` (46 tests). Options:

| Option | Effect |
| --- | --- |
| `--filter 'NativeWindow.PaneOverview*'` | Runs a subset (a `--gtest_filter` pattern). `'*'` also runs `Shaders/NativeRenderer.*`. |
| `--icd PATH/lvp.json` | Uses a software Vulkan driver (lavapipe) in place of the GPU |
| `--validation` | Enables the Vulkan validation layer and GPU logging |
| `--size 1920x1080` | Changes the virtual output size |
| `--timeout 400` | Stops the test binary after this many seconds (the default is 400) |

The log is long. Read it with `grep`, never in full.

### Expected skips in the headless run

These three tests need real compositor input, which only the acceptance runner below gives. Headless, they report
Skipped with "Run through native_window_acceptance.py for real compositor input". The two kitten tests also skip
when `kitten` is not installed.

- `NativeWindow.NativeWaylandFileDrop`
- `NativeWindow.KittyDragAndDropClient`
- `NativeWindow.KittyRemoteDragDirectory`

Every failure is real. Run a failed test again alone before you call it flaky, and say so in the report.
`TabDragOutPreservesLiveSplitAndMenuTargetsBackgroundTab` has failed one time and passed when run again.

## Full acceptance run (drag and drop)

```sh
python3 afterglow/tests/native_window_acceptance.py --binary build/afterglow/afterglow_native_tests
```

This runner opens a nested KWin as a window on the host X11 or XWayland display, and sends synthetic X events
(`XSendEvent`, not XTest) only to that window. It needs `kwin_wayland`, `DISPLAY`, libX11, `xprop`,
`dbus-run-session` and `kitten`. Use it for the drag-and-drop tests, or when the user asks
for a visible run.

## Add a native test

Tests live in `afterglow/tests/NativeWindowTest.cpp`. Wait on a condition with a deadline, never on a fixed sleep.
An animation test waits until the animation settles, and then checks the final state. Read the state through the
window's own accessors or the control snapshot, not through pixels, unless pixels are the point of the test.

## Shader output

For what a shader draws, use `afterglow_renderpng` (the `ag-shader-pack` skill). It is deterministic, and you can
read its PNG with the Read tool.
