<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Testing

Run commands from the repository root.
See [build instructions](BUILD.md) for dependencies and configuration.

## Regression tests

Build the test targets:

```sh
cmake --build build -j4
```

Run the CTest suite:

```sh
ctest --test-dir build --output-on-failure -j4
```

Without a Wayland display or a Vulkan device, the renderer tests and `PopupLook.*` report Skipped.
On a computer with Vulkan, a device that a test cannot make fails the test, with the reason from the driver.
The tests that draw with Vulkan take turns (the CTest `RESOURCE_LOCK` `gpu`), and the other tests run beside them.
Each makes a Vulkan device of its own. Several devices at once can use all the device memory (`VK_ERROR_OUT_OF_DEVICE_MEMORY`) when other programs hold most of it.
Some renderer tests also skip without a font that they need, or without 4× multisampling.
The fonts are DejaVu Sans, DejaVu Sans Mono, a color emoji font, a Nerd Font and an Arabic font.
Check the skipped count in the CTest summary.
Use `ctest -R PATTERN` to select tests by name.
See [compatibility](COMPATIBILITY.md) for protocol test names.

The tests of the pseudo-terminal library start with `PtyLib.`.
They check the spawn options that overlays use: standard input and output on pipes, apart from `/dev/tty`.

```sh
ctest --test-dir build --output-on-failure -R '^PtyLib\.'
```

The tests that start with `Pty.` are a different suite in the application's tests. They check the same library: deferred start, the environment and spawn failures.
`FzfPicker.WithFzfThePickComesFromTheOverlay` reports Skipped when `fzf` is not installed.
`RemoteCommandsTest.OpenSshForwardsAsTheDesignExpectsAndAglowRunWorksThroughIt` reports Skipped when `sshd`, `ssh` or `ssh-keygen` is not installed.
When the tools are there and its own sshd cannot start, the test fails.

## Build options for tests and tools

| Option | Default | Function |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Builds the unit and renderer tests. Requires GTest. |
| `AFTERGLOW_BUILD_TOOLS` | `OFF` | Builds the benchmarks, `afterglow_renderpng` and the headless conformance host. Adds the host's regression test and `aglow_icat_render` to CTest. |
| `AFTERGLOW_NATIVE_TESTS` | `OFF` | Adds the native window suite and `afterglow_transfer_kitty` to CTest with the label `native`. Requires Python 3 and the native test dependencies. |
| `AFTERGLOW_BUILD_FUZZERS` | `OFF` | Builds the libFuzzer targets for the terminal core, the protocol library and the image decoders of `aglow`. Requires clang for C and C++. Turns off `AFTERGLOW_BUILD_AGLOW`. |
| `AFTERGLOW_BUILD_AGLOW` | `ON` | Builds `aglow`. Requires the static C and C++ runtime libraries. Adds the [aglow tests](#aglow-tests) to CTest. When it is `OFF`, the `aglow dnd` tests, the `RunBinaryTest.*` tests and the `sshd` test of `RemoteCommandsTest.*` report Skipped. |

With `AFTERGLOW_NATIVE_TESTS=ON`, run only the native suite:

```sh
ctest --test-dir build -L native --output-on-failure
```

## Native test dependencies

The native window suite, the shader scene tests, the project and navigator tests and the native benchmark use a nested KWin desktop.
They need this software:

- `kwin_wayland`.
- An X11 or XWayland display, libX11 and `xprop`.
- `dbus-run-session`.
- ImageMagick `import`, for the tests that take screenshots.
- `kitten`, for the `Kitty*` drag-and-drop tests of the native window suite. Without it, those tests skip.

A drag-and-drop test of the native window suite that uses a client runs two times. It runs one time with `kitten dnd` and one time with `aglow dnd`.
The Wayland file drop test (`NativeWaylandFileDrop`) and the selection drag test
(`ASelectionDragsIntoAnotherPaneAndNotIntoItsOwn`) use no client. Each runs one time.
The `Aglow*` tests use the `aglow` of the build. In a build without `aglow`, they skip.

The tests send input to the nested KWin window as synthetic X events. They do not use XTest.

## Native window tests

Build the native target:

```sh
cmake --build build --target afterglow_native_tests
```

Run the native window suite:

```sh
python3 afterglow/tests/native_window_acceptance.py --binary build/afterglow/afterglow_native_tests
```

The runner creates an isolated nested KWin desktop on a private session bus.
See [native test dependencies](#native-test-dependencies) for the required software.
It uses the Basic control style and generic Qt platform theme.

| Option | Default | Function |
| --- | --- | --- |
| `--filter` | All tests | A GTest filter, for example `NativeWindow.AglowDragAndDropClient` |
| `--timeout` | 400 | Seconds for the whole run. The runner stops the suite at the limit and fails. |

See [desktop integration](DESKTOP.md#native-acceptance-coverage) for coverage and limits.

## Accessibility and downloads

Run the desktop suite:

```sh
python3 afterglow/tests/desktop_acceptance.py build/afterglow/afterglow
```

This suite also requires `python-dbus` and AT-SPI.
It uses temporary configuration, a private session bus and a separate accessibility registry.
The generic Qt platform theme keeps Save and Cancel inside the isolated desktop.
The suite checks public AT-SPI interfaces. It does not certify individual screen readers.

To test the upstream iTerm2 client, supply an inspected local copy:

```sh
python3 afterglow/tests/desktop_acceptance.py build/afterglow/afterglow --it2dl /path/to/it2dl
```

The repository does not include or automatically download that client.

## Developer tools and reference generation

Enable developer tools during configuration:

```sh
cmake -S . -B build -DAFTERGLOW_BUILD_TOOLS=ON
```

Build the tools before running their tests.
CTest runs the headless-tool regressions. To run them alone:

```sh
python3 tools/conformance/test_headless.py build/tools/conformance/afterglow_headless
```

Regenerate the configuration reference after schema changes:

```sh
python3 tools/generate-reference.py build/afterglow/afterglow
```

CTest runs `tools/generate-reference.py --check` as `afterglow_configuration_reference`.
The test fails when `docs/CONFIGURATION.md` is not what the schema of the build generates.
It needs Python 3.

### Conformance with esctest

The esctest runner drives the terminal core through `afterglow_headless`.
It needs a checkout of [esctest2](https://github.com/ThomasDickey/esctest2) and a build with developer tools.

```sh
tools/conformance/esctest.sh build /path/to/esctest2
```

The run passes when every failure is in `tools/conformance/esctest-expected-failures.txt`.
The script exits with a nonzero status on an unexpected failure.
Extra arguments go to esctest. With a filter, the script reports only unexpected failures.

### Fuzzing

The fuzz targets need clang for C and C++, and a separate build directory.
The targets compile the terminal core, the protocol library, zlib-ng, xxHash and the image decoders of `aglow` with the sanitizers.
Build only the fuzz targets in a fuzz build directory.
The commands below turn off the tests and build the three targets by name.
A fuzz build does not build `aglow`, because a static program cannot use the sanitizer runtimes.

| Target | Input |
| --- | --- |
| `afterglow_vtfuzz` | A byte stream to the terminal core, with control records between reads. |
| `afterglow_protofuzz` | The protocol readers for remote input. The first byte selects the reader. |
| `afterglow_imagefuzz` | An image file to the image decoders of `aglow`: the patched stb_image and the GIF reader of `aglow`. |

Build the targets:

```sh
cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DAFTERGLOW_BUILD_FUZZERS=ON -DBUILD_TESTING=OFF
cmake --build build-fuzz --target afterglow_vtfuzz afterglow_protofuzz afterglow_imagefuzz
```

Run the terminal core target:

```sh
mkdir -p build-fuzz/corpus
./build-fuzz/tools/fuzz/afterglow_vtfuzz -max_total_time=300 -max_len=65536 -dict=tools/fuzz/vt.dict build-fuzz/corpus/
```

Run the protocol target with its own corpus:

```sh
mkdir -p build-fuzz/corpus-proto
./build-fuzz/tools/fuzz/afterglow_protofuzz -max_total_time=300 -max_len=65536 build-fuzz/corpus-proto/
```

Run the image target with its own corpus:

```sh
mkdir -p build-fuzz/corpus-image
./build-fuzz/tools/fuzz/afterglow_imagefuzz -max_total_time=300 -max_len=65536 build-fuzz/corpus-image/
```

A run passes when the fuzzer stops at the time limit without a crash report.

### Unicode tables

Regenerate the Unicode property tables after a Unicode version change:

```sh
python3 -I tools/gen_unicode_tables.py UCD_DIR libs/afterglow-vt/src/UnicodeTables.inc
```

`UCD_DIR` holds `EastAsianWidth.txt`, `DerivedCoreProperties.txt`, `GraphemeBreakProperty.txt`, `emoji-data.txt` and `DerivedGeneralCategory.txt` in one directory.
Run the CTest suite after the change.

## Shader scenes

Run the native parameter and scene tests:

```sh
python3 afterglow/tests/shader_parameters_smoke.py build/afterglow/afterglow
python3 afterglow/tests/scene_acceptance.py build/afterglow/afterglow
```

These tests need the [native test dependencies](#native-test-dependencies).
They also need the `aglow` of the build next to `afterglow`, because they read the state of a pane with `aglow run`.
Without it, they stop with an error.
The combined scene test also requires CAVA, pactl and paplay.
Each test runs again on a private session bus and copies the bundled shader packs into its temporary configuration.
It prints the directory for its log, screenshots and results.

| Option | Default | Function |
| --- | --- | --- |
| `--artifacts DIR` | A new temporary directory | The directory for the log, the screenshots and the results |
| `--timeout` | 300 seconds, and 600 seconds for `scene_acceptance.py` | The time for the whole run. At the limit, the runner stops the test and exits with status 124. |

See [Phosphor performance](../data/examples/phosphor/PERFORMANCE.md) for benchmark commands and measurement limits.

## Projects and the project navigator

Run the project and pane transfer test:

```sh
python3 afterglow/tests/project_smoke.py build/afterglow/afterglow
```

Run the project navigator package tests:

```sh
python3 afterglow/tests/navigator_smoke.py build/afterglow/afterglow
python3 afterglow/tests/navigator_ui_smoke.py build/afterglow/afterglow
```

These tests need the [native test dependencies](#native-test-dependencies) and Bash.
`navigator_smoke.py` also needs the `aglow` of the build next to `afterglow`, because it runs the commands of the package with `aglow run`.
Without it, the test stops with an error.
`navigator_ui_smoke.py` also needs `git` and `fd`.
It keeps its screenshots and log in a new temporary directory, or in the directory that `--artifacts` gives.
`navigator_smoke.py` and `navigator_ui_smoke.py` take `--timeout`, the time for the whole run in seconds. The default is 300.
At the limit, the runner stops the test and exits with status 124.
See [projects](PROJECTS.md) and the [project navigator](../data/examples/packages/project-navigator/README.md) for what the tests cover.

## Native performance measurements

Configure and build the benchmark:

```sh
cmake -S . -B build -DAFTERGLOW_BUILD_TOOLS=ON
cmake --build build --target afterglow_native_bench -j4
```

Run the repeated workloads:

```sh
python3 tools/bench/native_benchmark.py build/afterglow/afterglow_native_bench ~/afterglow-native-performance
```

The runner needs the [native test dependencies](#native-test-dependencies).
Install Monaspace Neon Var to reproduce the recorded font conditions.
NVIDIA process memory sampling is optional.
The output directory must not contain earlier results.
Use persistent storage to keep results through a restart.
The runner starts itself again on a private session bus.
Each case uses a new application process and temporary configuration.
The configuration sets Monaspace Neon Var at 11 points and keeps the cursor, scroll and split animations on.
It turns off the overview, pane effect and tab switch packs.
The windows use scale 1.

| Option | Default | Limits |
| --- | --- | --- |
| `--repeats` | 3 | 1 to 10 |
| `--duration` | 15 seconds for each timed case | 1 to 120 seconds |
| `--samples` | 100 input acknowledgements | 1 to 1000 |
| `--sizes` | `1920x1080 3840x2160` | 720×480 to 7680×4320. Give each size only once. |
| `--cases` | All workloads | See the [workloads](PERFORMANCE.md#workloads) |

A value outside these limits stops the runner with an error before the measurement starts.
The test does not change your terminal configuration.
Close other heavy applications before measurement.
Do not build or run other tests during measurement.

Use `--cases` to select workloads and `--sizes` to select window sizes:

```sh
python3 tools/bench/native_benchmark.py build/afterglow/afterglow_native_bench ~/afterglow-input-performance \
    --cases input input-load --sizes 1920x1080 --repeats 3 --samples 100
```

See [performance measurements](PERFORMANCE.md) for metric definitions and limits.

Archive a completed run:

```sh
python3 tools/bench/summarize_native.py ~/afterglow-native-performance ~/afterglow-native-baseline
```

This command writes `.summary.json` and `.raw.json.gz` files.
It rejects incomplete runs, missing repeats and inconsistent sample counts.
The summary gives the median and range of each per-run statistic.
The compressed archive keeps all samples and environment metadata.

## Kitty desktop protocols

Run the file-transfer and drag-and-drop tests of the service and of its clients:

```sh
ctest --test-dir build --output-on-failure -R '^(Transfer|TransferQuestion|TransferUmask|Client|Dnd|DndSource|DragTrees|RemoteTreeLinks|Answers|AglowDrop|AglowDrag|AglowOverwrite)\.'
```

`TransferQuestion.*` checks the paths that the permission dialog shows.
`TransferUmask.*` checks the mode of a received file.
The mode comes from the umask that the GUI thread had when it made the transfer service.
A later change of the umask does not change the mode.

Run the tests of the protocol library, which has these formats and the other wire formats.
The names of these tests start with `Proto.`.

```sh
ctest --test-dir build --output-on-failure -R '^Proto\.'
```

Two test suites have the name `Client`:

| Tests | Binary | What they run |
| --- | --- | --- |
| `Client.*` | `afterglow_transfer_tests` | The transfer client of the protocol library, which `aglow transfer` uses, against the service. The tests send and receive a tree with links and a delta. |
| `Proto.Client.*` | `afterglow_proto_tests` | The parts of the transfer client, and whole sessions against a terminal that the test plays. |

The `Proto.Hostile.*` tests run the client against a hostile terminal that the test plays.
Its listings try to write through links, to replace files outside the destination, and to send more data than they announce.

The `TransferLimits.*` tests fill the descriptor limit of file transfers, which is half of the soft `RLIMIT_NOFILE`.
CTest runs them as `afterglow_transfer_descriptor_limits`, with a soft limit of 64.
That entry fails when one of the tests skips or when no test runs, because then the limit did not apply.
In other runs, they report Skipped.

CTest runs the relay tests below as `afterglow_transfer_kitten_relay` and `afterglow_transfer_aglow_relay`.
`afterglow_transfer_kitten_relay` reports Skipped when `kitten` is not installed.
With `AFTERGLOW_NATIVE_TESTS=ON`, CTest also runs the `--kitty` test as `afterglow_transfer_kitty`.

Run interoperability tests with the installed `kitten` client:

```sh
python3 afterglow/tests/kitty_transfer_interop.py --relay build/afterglow/tests/afterglow_transfer_tests
```

Kitty 0.49.2 has a receive-side empty-file defect.
For that version, add `--allow-client-empty-file-bug` to test the remaining transfers.
The service tests check empty-file completion separately.

The same script runs `aglow` in place of `kitten`, in a pseudo-terminal that it creates:

```sh
python3 afterglow/tests/kitty_transfer_interop.py --relay build/afterglow/tests/afterglow_transfer_tests --aglow build/afterglow/aglow
```

With `--kitty`, the script runs `aglow` against Kitty itself.
Each transfer is a Kitty window in a headless nested KWin on a private session bus.
Kitty and the client share a password, so Kitty shows no permission prompt.
The script gives Kitty, KWin and the bus a temporary home directory.
Without Kitty, `kwin_wayland`, `dbus-run-session` or `XDG_RUNTIME_DIR`, the script prints a message and exits with code 77.

```sh
python3 afterglow/tests/kitty_transfer_interop.py --kitty --aglow build/afterglow/aglow
```

`AglowDrop.ReadsARemoteTreeThroughTheTerminal` runs `aglow dnd` in a pseudo-terminal and drops a directory tree on it.
The service reports the drop as remote, so `aglow dnd` reads the tree through directory handles.
`AglowDrop.WithoutAMachineIdTheFilesAreReadThroughTheTerminal` hides `/etc/machine-id` from `aglow dnd` with `unshare`.
Without `unshare` and user namespaces, it skips.
`AglowDrop.ADirectoryThatHoldsTheDropDirectoryIsRefused` drops a directory that holds the drop directory, and checks that `aglow dnd` writes nothing.
`AglowDrag.ACanceledMoveKeepsTheFiles` plays the terminal for a remote drag, and checks that only a finished move deletes the files.
The `AglowOverwrite.*` tests answer the question of `--confirm-drop-overwrite`, and let it expire.

The native window suite uses `aglow dnd` and `kitten dnd` for text drags and remote directory drags.
See [native test dependencies](#native-test-dependencies).

## Control and script command tests

Run the tests of the control socket, `afterglow ctl`, the `commands` target and `aglow run`:

```sh
ctest --test-dir build --output-on-failure -R '^(ControlSocketTest|CtlBinaryTest|CtlOverlayTest|CommandSocketTest|RunBinaryTest|AfterglowRunWithoutAglowTest|RemoteCommandsTest|Proto\.RunClient|Proto\.RunInput|Proto\.RunRemote|Proto\.Socket|Proto\.Remote|Proto\.Ipc)\.|RemoteSocket|aglow_run_pty|aglow_tmux_pty'
```

| Tests | Binary | What they run |
| --- | --- | --- |
| `ControlSocketTest.*` | `afterglow_control_tests` | The `control` target: calls and errors, `overlay` and its `overlayInput` parts, the socket lookup, stale sockets and merged events |
| `CtlBinaryTest.*` and `CtlOverlayTest.*` | `afterglow_command_tests` | `afterglow ctl` against a socket that the test makes: usage errors, and `afterglow ctl overlay` with its options and standard input |
| `CommandSocketTest.*` | `afterglow_command_tests` | The `commands` target of the socket: the list, runs, standard input, flow control, cancel and the limits |
| `RunBinaryTest.*` | `afterglow_command_tests` | `aglow run` against a socket that the test makes. They check the exit status, the output, standard input, the pane, signals and the socket lookup. |
| `AfterglowRunWithoutAglowTest.*` | `afterglow_command_tests` | `afterglow run` in a build without `aglow`. The test checks the error and exit status 127. |
| `Proto.RunClient.*` and `Proto.RunInput.*` | `afterglow_proto_tests` | The client of `aglow run` against a server that the test plays, and the parts of standard input that it sends |
| `Proto.RunRemote.*` | `afterglow_proto_tests` | How the client of `aglow run` uses the answer of the terminal to OSC 24569, and its messages when there is no socket |
| `Proto.Socket.*` | `afterglow_proto_tests` | The socket lookup of `afterglow ctl` and `aglow run` |
| `Proto.Remote.*` | `afterglow_proto_tests` | OSC 24569, the remote directory and `aglow remote-prune` |
| `RemoteCommandsTest.*` | `afterglow_remote_tests` | The sockets of remote commands, the probe of `afterglow ssh` with a fake `ssh`, and an acceptance test with `sshd` |
| `Proto.Ipc.*` | `afterglow_proto_tests` | The phosphor-ipc client of the protocol library |
| `Extensions.*RemoteSocket*` | `afterglow_vt_tests` | The answer of the pane to OSC 24569: the path or the reason, and the order of the answer before DA1 |
| `aglow_run_pty` and `aglow_tmux_pty` | Python scripts | The OSC 24569 query of `aglow run` in a pseudo-terminal, also in a private tmux server. See [aglow tests](#aglow-tests) |

`NativeWindow.AnOverlaysProgramGetsTheRemoteSocketOfItsHost` in the [native window suite](#native-window-tests) checks the answer to OSC 24569 in an overlay.

`RunBinaryTest.AfterglowRunRunsAglowRun` runs `aglow run` through `afterglow run`.
In a build without `aglow`, the `RunBinaryTest.*` tests and `RemoteCommandsTest.OpenSshForwardsAsTheDesignExpectsAndAglowRunWorksThroughIt` report Skipped.
In a build with `aglow`, `AfterglowRunWithoutAglowTest.SaysThatTheBuildHasNoAglow` reports Skipped.
The tests give `aglow run` their own `XDG_RUNTIME_DIR` and socket. They never reach the Afterglow that runs them.

The acceptance test of remote commands starts two `sshd` as you, on free ports of 127.0.0.1.
It makes host and client keys, `sshd_config` and an ssh client configuration in a temporary directory.
Every `ssh` of the test takes that configuration with `-F`, with no agent and its own known hosts.
The "remote" side is this computer, with a `HOME` and `XDG_RUNTIME_DIR` of the test.
The test checks the OpenSSH behavior that remote commands depend on, then runs `aglow run` in an `afterglow ssh` session of a pane.
It stops the `sshd` and removes the directory at the end.
When the test binary dies first, the kernel stops the `sshd`.

`RemoteCommandsTest.ARunWhoseHandlerTurnsTheSettingOffEndsWithItsSocket` turns the setting off inside the call of a run.
A socket that went away inside its own call is a use of freed memory, which a normal build does not always show.
Run the test in the `debug` preset, which has the sanitizers, to see it every time.

## Shell completion tests

CTest runs `afterglow/tests/completion_test.py` as `afterglow_shell_completion`.
It checks the completions of `afterglow` and `aglow` in `data/completions` for Bash, Zsh and Fish.
The test uses stub programs in place of `afterglow` and `aglow`, so no real Afterglow runs.
It checks Bash without the `bash-completion` package.
When that package is installed, it also checks Bash with the package.
A shell that is not installed is left out. Without Bash, Zsh and Fish, the test reports Skipped.
It needs Python 3.

## aglow tests

A build with `AFTERGLOW_BUILD_AGLOW=ON` adds these tests to CTest:

| Test | Function | Requirement |
| --- | --- | --- |
| `aglow_version` | Checks the output of `aglow --version` | None |
| `afterglow_runs_aglow`, `afterglow_passes_aglow_arguments`, `afterglow_passes_aglow_status` | Run `aglow` through `afterglow icat` and `afterglow transfer`, and check its usage text and exit status | None |
| `afterglow_without_aglow` | Runs a copy of `afterglow` that has no `aglow` next to it, and checks the error and status 127 | None |
| `afterglow_aglow_not_executable` | Runs a copy of `afterglow` next to an `aglow` that it cannot execute, and checks the error and status 126 | None |
| `aglow_run_usage`, `afterglow_runs_aglow_run` | Check the usage of `aglow run`, and of `afterglow run`, which runs it | None |
| `aglow_no_command`, `aglow_unknown_command`, `aglow_help_unknown_command` | Run `aglow` with no command, an unknown command and `help` of an unknown command, and check the usage text and exit status 2 | None |
| `aglow_remote_prune_help`, `aglow_remote_prune_usage`, `aglow_remote_prune_missing` | Check `aglow remote-prune --help` (status 0), `aglow remote-prune` with no directory (status 2) and a directory that does not exist (status 1) | None |
| `Aglow.*` | The input parser and the command line of `aglow`, in `aglow_tests` | GTest |
| `afterglow_transfer_aglow_relay` | Runs `aglow transfer` in a pseudo-terminal against the file-transfer service, also with `--confirm-paths` answered with y and n | Python 3 |
| `afterglow_transfer_kitty` | Runs `aglow transfer` against Kitty | Python 3, `AFTERGLOW_NATIVE_TESTS`, Kitty and the native test dependencies |
| `aglow_static` | Checks that `readelf -d` lists no shared library for `aglow` | `readelf`, and a static `aglow` |
| `aglow_only_build` | Configures `aglow` alone (`AFTERGLOW_AGLOW_ONLY`), with `find_package(Qt6)` disabled and no Phosphor directory. Builds it and installs it into the build directory. Checks that the installation holds only `bin/aglow`, its license notices and its completions. Uses the compilers, the build type and the flags of the build, also the flags of each build type. Has the label `slow` | A full build, and the source archives of zlib-ng and xxHash or a network connection |
| `aglow_clipboard_pty` | Plays a terminal for `aglow clipboard` in a pseudo-terminal | Python 3 |
| `aglow_run_pty` | Plays a terminal and a terminal socket for the OSC 24569 query of `aglow run`. Checks when `aglow run` does not ask, the modes of the terminal after signals and `Ctrl+C`, and what `aglow remote-prune` removes | Python 3 |
| `aglow_tmux_pty` | Runs `aglow icat` and `aglow run` in a private tmux server (`tmux -L`, its own configuration and socket directory) in a pseudo-terminal, with `allow-passthrough` on and off | Python 3 and tmux. Without tmux, it reports Skipped |
| `aglow_clipboard_cases` | Checks the arguments that `aglow clipboard` refuses, a new output file, and the timeout of an OSC 52 read | Python 3, and a kernel that shows a process that waits in `open()` |
| `aglow_icat_decode` | Sends crafted images to `aglow icat` and checks the pixels that it sends, or its error | Python 3, and a user that cannot read a directory of mode 0 |
| `aglow_icat_render` | Draws the output of `aglow icat` with `afterglow_renderpng` | Python 3, `AFTERGLOW_BUILD_TOOLS`, a Wayland display and a Vulkan device |

Without `readelf`, the configuration prints a message and leaves out `aglow_static`.
`aglow_only_build` compiles zlib-ng and xxHash again, which takes some minutes.
Run the suite without it:

```sh
ctest --test-dir build --output-on-failure -LE slow
```

A [build of aglow alone](BUILD.md#build-aglow-alone) has the `Proto.*` tests and these tests of the table: `aglow_version`, `aglow_run_usage`, `aglow_no_command`, `aglow_unknown_command`, `aglow_help_unknown_command`, the three `aglow_remote_prune_*` tests, `Aglow.*`, `aglow_static`, `aglow_clipboard_pty`, `aglow_run_pty`, `aglow_tmux_pty`, `aglow_clipboard_cases` and `aglow_icat_decode`.
It has no test that runs `afterglow`.

The Python test scripts need Python 3.9 or later.
Without a Wayland display or a Vulkan device, `aglow_icat_render` reports Skipped.
`aglow_clipboard_cases` reports Skipped when the kernel does not show a process that waits in `open()`. It runs its other cases first.
`aglow_icat_decode` reports Skipped when its user can read a directory of mode 0, as root can. It runs its other cases first.
`aglow_icat_decode` and `aglow_icat_render` run `aglow` without `TMUX`, so the tests do not change the settings of a tmux that runs them.
`aglow_icat_render` also runs `afterglow_renderpng` without `TMUX`.
It runs `afterglow_renderpng` with `QT_QPA_PLATFORMTHEME=generic`, so the theme plugin of the desktop does not load.
`aglow_run_pty` and `aglow_tmux_pty` run `aglow` without `TMUX`, `TMUX_PANE`, `AFTERGLOW_SOCKET`, `AFTERGLOW_PANE_ID` and `WAYLAND_DISPLAY`, with an `XDG_RUNTIME_DIR` of their own.
Thus they do not reach a tmux or an Afterglow that runs them. `aglow_tmux_pty` starts its own tmux server with `tmux -L` and its own `TMUX_TMPDIR`.

`aglow_icat_render` checks a PNG image, a mirrored image, an image placed with `--place`, Unicode placeholders and each frame of an animated GIF.
It also gives `aglow icat` a PNG header of 12,000 × 12,000 pixels.
The test checks that `aglow icat` exits with status 1, names the size, and sends no graphics command.
