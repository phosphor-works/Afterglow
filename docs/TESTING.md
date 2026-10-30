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

Renderer tests require a working Vulkan driver.
Use `ctest -R PATTERN` to select tests by name.
See [compatibility](COMPATIBILITY.md) for protocol test names.

## Native window tests

Build the native target:

```sh
cmake --build build --target afterglow_native_tests
```

Run the native window suite:

```sh
python3 afterglow/tests/native_window_acceptance.py --binary build/afterglow/afterglow_native_tests
```

The runner creates an isolated nested KWin desktop.
It requires X11 or XWayland, D-Bus and XTest.
It uses the Basic control style and generic Qt platform theme.
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
Run the headless-tool regressions:

```sh
python3 tools/conformance/test_headless.py build/tools/conformance/afterglow_headless
```

Regenerate the configuration reference after schema changes:

```sh
python3 tools/generate-reference.py build/afterglow/afterglow
```

## Shader scenes

Run the native parameter and scene tests:

```sh
python3 afterglow/tests/shader_parameters_smoke.py build/afterglow/afterglow
python3 afterglow/tests/scene_acceptance.py build/afterglow/afterglow
```

These tests require KWin, X11 or XWayland, XTest and ImageMagick.
The combined scene test also requires CAVA, pactl and paplay.
See [Phosphor performance](../data/examples/phosphor/PERFORMANCE.md) for benchmark commands and measurement limits.
