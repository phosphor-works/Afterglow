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
`afterglow/src/TerminalWindow*`, `Pane*`, `SplitTree*`, `KeyMapping*`, `chrome/`, `desktop/`, `tools/aglow/Dnd.cpp`
or the scripting glue that drives windows. Unit tests do not cover these paths.

Never test against the user's desktop session. The user runs Afterglow as their terminal, and a test that types,
closes windows or kills processes there damages their work.

## Headless run (the default)

1. Build the test binary:

   ```sh
   systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 \
       cmake --build build -j6 --target afterglow_native_tests
   ```

2. Run it in a headless nested KWin. The runner starts itself under `dbus-run-session`, so KWin gets a private
   D-Bus. The test binary runs under a second `dbus-run-session`, with a private D-Bus of its own:

   ```sh
   systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 \
       python3 .claude/skills/ag-verify-live/headless_native.py > <scratchpad>/native.log 2>&1; echo $?
   grep -E '^\[  (FAILED|PASSED) |^\[==' <scratchpad>/native.log
   ```

The default filter is `NativeWindow.*:KeyMapping.*`: every native test but `Shaders/NativeRenderer.*`.
`KeyMapping.*` runs here and not in the default CTest suite, because it maps keys through the keymap of the
compositor's seat. With `AFTERGLOW_NATIVE_TESTS=ON`, CTest runs the whole binary in a nested KWin, `KeyMapping.*`
included.
Options:

| Option | Default | Effect |
| --- | --- | --- |
| `--filter 'NativeWindow.PaneOverview*'` | `NativeWindow.*:KeyMapping.*` | Runs a subset (a `--gtest_filter` pattern). `'*'` also runs `Shaders/NativeRenderer.*`. |
| `--binary PATH` | `build/afterglow/afterglow_native_tests` | Runs another test binary, such as one from another build directory |
| `--icd PATH/lvp.json` | The host driver | Uses a software Vulkan driver (lavapipe) in place of the GPU |
| `--validation` | Off | Enables the Vulkan validation layer and GPU logging |
| `--size 1920x1080` | `1100x750` | Changes the virtual output size |
| `--timeout 400` | 400 | Stops the test binary after this many seconds and 10 more, and reports status 124 |
| `--platform-theme kde` | `generic` | Uses the native file dialog of Plasma (`plasma-integration`). Run only the dialog tests with it (`--filter 'NativeWindow.*Dialog*'`): the other tests expect `generic`. |

The log is long. Read it with `grep`, never in full.
The memory cap of `systemd-run` keeps systemd-oomd from killing the user's Afterglow (see `ag-build`).

### Expected skips in the headless run

These five tests need real compositor input, which only the acceptance runner below gives. Headless, they report
Skipped with "Run through native_window_acceptance.py for real compositor input".

- `NativeWindow.NativeWaylandFileDrop`
- `NativeWindow.KittyDragAndDropClient` and `NativeWindow.AglowDragAndDropClient`
- `NativeWindow.KittyRemoteDragDirectory` and `NativeWindow.AglowRemoteDragDirectory`

Each client drag-and-drop test runs two times, with two clients. `NativeWaylandFileDrop` uses no client and runs
one time. The `Kitty*` tests run `kitten dnd`, and skip when `kitten` is not installed. The `Aglow*` tests run the
`aglow dnd` of the build, and skip in a build without `aglow` (`AFTERGLOW_BUILD_AGLOW=OFF`).

`NativeWindow.APointerThatNeverEnteredAWindowIsOnNoItemOfItsDocks` skips when the compositor's pointer enters its
new window, because the test needs a window that the pointer never entered.

Every failure is real. Run a failed test again alone before you call it flaky, and say so in the report.
A test that failed once can race the chrome scene, which a window makes in the background. A test that sends
pointer input to the tab bar or an overlay must first wait for `ChromeLayer::rootItem()` and one frame.

## Full acceptance run (drag and drop)

```sh
systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 \
    python3 afterglow/tests/native_window_acceptance.py --binary build/afterglow/afterglow_native_tests
```

This runner opens a nested KWin as a window on the host X11 or XWayland display, and sends synthetic X events
(`XSendEvent`, not XTest) only to that window. Use it for the drag-and-drop tests, or when the user asks for a
visible run. It needs this software:

- `kwin_wayland` and `dbus-run-session`.
- An X11 or XWayland display (`DISPLAY`), libX11 and `xprop`.
- `kitten`, for the `Kitty*` drag-and-drop tests only.

The nested KWin is a window on the user's desktop, so the user's own pointer reaches it. A drag that gets moves
from the real pointer as well as from the runner fails: the drop target gets a leave and an enter for each move, or
the press never arrives. To prove such a failure, log the position of each drag event in `TerminalWindow::event`
for a run. Moves that the runner did not send (10 steps on a line) come from the host. Ask the user to keep the
pointer off the window, and run again.

## Add a native test

Tests live in `afterglow/tests/NativeWindowTest.cpp`. Wait on a condition with a deadline, never on a fixed sleep.
An animation test waits until the animation settles, and then checks the final state. Read the state through the
window's own accessors or the control snapshot, not through pixels, unless pixels are the point of the test.

## Shader output

For what a shader draws, use `afterglow_renderpng` (the `ag-shader-pack` skill). It is deterministic, and you can
read its PNG with the Read tool.
