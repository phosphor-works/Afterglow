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
| `AFTERGLOW_NATIVE_TESTS` | `OFF` | Adds the native window suite to CTest with the label `native`. Requires Python 3 and the native test dependencies. |
| `AFTERGLOW_BUILD_FUZZERS` | `OFF` | Builds the libFuzzer target for the terminal core. Requires clang. |

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
- `kitten`, for the Kitty drag-and-drop tests of the native window suite. Without it, those tests skip.

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
| `--filter` | All tests | A GTest filter, for example `NativeWindow.KittyDragAndDropClient` |
| `--timeout` | 110 | Seconds for the whole run. The runner stops the suite at the limit and fails. |

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

The fuzz target needs clang and a separate build directory:

```sh
cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DAFTERGLOW_BUILD_FUZZERS=ON
cmake --build build-fuzz --target afterglow_vtfuzz
mkdir -p corpus
./build-fuzz/tools/fuzz/afterglow_vtfuzz -max_total_time=300 -max_len=65536 -dict=tools/fuzz/vt.dict corpus/
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

Run the file-transfer and drag-and-drop service tests:

```sh
ctest --test-dir build --output-on-failure -R 'Transfer\.|Dnd\.'
```

Run interoperability tests with the installed `kitten` client:

```sh
python3 afterglow/tests/kitty_transfer_interop.py --relay build/afterglow/tests/afterglow_transfer_tests
```

Kitty 0.49.2 has a receive-side empty-file defect.
For that version, add `--allow-client-empty-file-bug` to test the remaining transfers.
The service tests check empty-file completion separately.

The native window suite also uses `kitten dnd` for text drags and remote directory drags.
Without `kitten`, those tests skip.
