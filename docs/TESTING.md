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

Without a Vulkan device, the renderer tests report Skipped.
Some renderer tests also skip without a font that they need, or without 4× multisampling.
The fonts are DejaVu Sans, DejaVu Sans Mono, a color emoji font, a Nerd Font and an Arabic font.
Check the skipped count in the CTest summary.
Use `ctest -R PATTERN` to select tests by name.
See [compatibility](COMPATIBILITY.md) for protocol test names.

## Build options for tests and tools

| Option | Default | Function |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Builds the unit and renderer tests. Requires GTest. |
| `AFTERGLOW_BUILD_TOOLS` | `OFF` | Builds the benchmarks, `afterglow_renderpng` and the headless conformance host. Adds the host's regression test to CTest. |
| `AFTERGLOW_NATIVE_TESTS` | `OFF` | Adds the native window suite and `afterglow_transfer_kitty` to CTest with the label `native`. Requires Python 3 and the native test dependencies. |
| `AFTERGLOW_BUILD_FUZZERS` | `OFF` | Builds the libFuzzer targets for the terminal core, the protocol library and the image decoders of `aglow`. Requires clang for C and C++. Turns off `AFTERGLOW_BUILD_AGLOW`. |
| `AFTERGLOW_BUILD_AGLOW` | `ON` | Builds `aglow`. Requires the static C and C++ runtime libraries. Adds the [aglow tests](#aglow-tests) to CTest. When it is `OFF`, the `aglow dnd` tests report Skipped. |

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
The Wayland file drop test (`NativeWaylandFileDrop`) uses no client. It runs one time.
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
The combined scene test also requires CAVA, pactl and paplay.
Each test runs again on a private session bus and copies the bundled shader packs into its temporary configuration.
It prints the directory for its log, screenshots and results.
The default is a new temporary directory. Use `--artifacts DIR` to select another directory.
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
`navigator_ui_smoke.py` also needs `git` and `fd`.
It keeps its screenshots and log in a new temporary directory, or in the directory that `--artifacts` gives.
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
ctest --test-dir build --output-on-failure -R '^(Transfer|Client|Dnd|DndTree|AglowDrop|AglowDrag|AglowOverwrite)\.'
```

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

## aglow tests

A build with `AFTERGLOW_BUILD_AGLOW=ON` adds these tests to CTest:

| Test | Function | Requirement |
| --- | --- | --- |
| `aglow_version` | Checks the output of `aglow --version` | None |
| `afterglow_runs_aglow`, `afterglow_passes_aglow_arguments`, `afterglow_passes_aglow_status` | Run `aglow` through `afterglow icat` and `afterglow transfer`, and check its usage text and exit status | None |
| `afterglow_without_aglow` | Runs a copy of `afterglow` that has no `aglow` next to it, and checks the error and status 127 | None |
| `afterglow_aglow_not_executable` | Runs a copy of `afterglow` next to an `aglow` that it cannot execute, and checks the error and status 126 | None |
| `Aglow.*` | The input parser and the command line of `aglow`, in `aglow_tests` | GTest |
| `afterglow_transfer_aglow_relay` | Runs `aglow transfer` in a pseudo-terminal against the file-transfer service | Python 3 |
| `afterglow_transfer_kitty` | Runs `aglow transfer` against Kitty | Python 3, `AFTERGLOW_NATIVE_TESTS`, Kitty and the native test dependencies |
| `aglow_static` | Checks that `readelf -d` lists no shared library for `aglow` | `readelf`, and a static `aglow` |
| `aglow_clipboard_pty` | Plays a terminal for `aglow clipboard` in a pseudo-terminal | Python 3 |
| `aglow_clipboard_cases` | Checks the arguments that `aglow clipboard` refuses, a new output file, and the timeout of an OSC 52 read | Python 3 |
| `aglow_icat_decode` | Sends crafted images to `aglow icat` and checks the pixels that it sends, or its error | Python 3 |
| `aglow_icat_render` | Draws the output of `aglow icat` with `afterglow_renderpng` | Python 3, `AFTERGLOW_BUILD_TOOLS`, a Wayland display and a Vulkan device |

Without `readelf`, the configuration prints a message and leaves out `aglow_static`.
The Python test scripts need Python 3.9 or later.
Without a Wayland display or a Vulkan device, `aglow_icat_render` reports Skipped.
`aglow_icat_decode` and `aglow_icat_render` run `aglow` without `TMUX`, so the tests do not change the settings of a tmux that runs them.
`aglow_icat_render` also runs `afterglow_renderpng` without it.
It runs `afterglow_renderpng` with `QT_QPA_PLATFORMTHEME=generic`, so the theme plugin of the desktop does not load.

`aglow_icat_render` checks a PNG image, a mirrored image, an image placed with `--place`, Unicode placeholders and each frame of an animated GIF.
It also gives `aglow icat` a PNG header of 12,000 × 12,000 pixels.
The test checks that `aglow icat` exits with status 1, names the size, and sends no graphics command.
