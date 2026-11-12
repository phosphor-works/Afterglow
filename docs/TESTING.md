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

Without a Wayland display or a Vulkan device, the renderer tests, `PopupLook.*` and `PointerMap.*` report Skipped.
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
They also check the spawn options that mux sessions use: a child environment without the environment of the parent.
Other tests check the signals of a new child, a failed `fork()` and the parser of `/proc/PID/stat`.

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
| `AFTERGLOW_BUILD_TOOLS` | `OFF` | Builds the benchmarks, `afterglow_renderpng`, the headless conformance host and the [TUI fuzzer](#tui-fuzzing). Adds the regression tests of the host and the fuzzer, [`afterglow_kitten_icat`](#kitten-icat) and `aglow_icat_render` to CTest. |
| `AFTERGLOW_NATIVE_TESTS` | `OFF` | Adds the native window suite to CTest with the label `native`, and with `AFTERGLOW_BUILD_AGLOW` also `afterglow_transfer_kitty`. Requires Python 3 and the native test dependencies. |
| `AFTERGLOW_BUILD_FUZZERS` | `OFF` | Builds the libFuzzer targets for the terminal core, the protocol library, the image decoders that `aglow` and `afterglow-mux` share, and the attach stream of `afterglow-mux`. Requires clang for C and C++. Turns off `AFTERGLOW_BUILD_AGLOW`. |
| `AFTERGLOW_BUILD_AGLOW` | `ON` | Builds `aglow`. Requires the static C and C++ runtime libraries (see [BUILD.md](BUILD.md#required-software)). Adds the [aglow tests](#aglow-tests) to CTest. When it is `OFF`, the `aglow dnd` tests, the `RunBinaryTest.*` tests and the `sshd` test of `RemoteCommandsTest.*` report Skipped. |
| `AFTERGLOW_BUILD_VTDIFF` | `OFF` | Builds `afterglow_vtdiff`, which compares the terminal core with libghostty-vt. Requires libghostty-vt 1.0 in `CMAKE_PREFIX_PATH`. See [Differences with libghostty-vt](#differences-with-libghostty-vt). |

With `AFTERGLOW_NATIVE_TESTS=ON`, run only the native suite:

```sh
ctest --test-dir build -L native --output-on-failure
```

## Sanitizer build

The `debug` preset builds Afterglow and its tests with AddressSanitizer and UndefinedBehaviorSanitizer in `build-debug`.
It compiles at `-O1`, as the sanitizer documentation recommends. At `-O0` the tests are too slow for their waits.
It turns off `-Wmaybe-uninitialized`, because GCC gives false warnings for instrumented code.
Run the sanitizer build before a pull request that changes a parser, a protocol or the memory of a pane.

1. Configure the build directory with the Phosphor checkout of `build` (see `AFTERGLOW_PHOSPHOR_LIBS_DIR` in `build/CMakeCache.txt`):

   ```sh
   cmake --preset debug -DAFTERGLOW_PHOSPHOR_LIBS_DIR=../PlasmaZones/phosphor-libs
   ```

2. Build it with two jobs. Each sanitizer compile needs more memory than a normal compile:

   ```sh
   systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 cmake --build build-debug -j2
   ```

3. Run the suite for memory errors and undefined behavior:

   ```sh
   ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
       ctest --test-dir build-debug --output-on-failure -j4
   ```

4. Run the suite for leaks, without the tests that have the label `gpu`:

   ```sh
   ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=suppressions=$PWD/tools/sanitizers/lsan.supp \
       ctest --test-dir build-debug -LE gpu --output-on-failure -j4
   ```

5. Run the native window tests on the sanitizer build:

   ```sh
   ASAN_OPTIONS=detect_leaks=0 python3 .claude/skills/ag-verify-live/headless_native.py \
       --binary build-debug/afterglow/afterglow_native_tests --timeout 3600
   ```

`UBSAN_OPTIONS=halt_on_error=1` makes a test fail at the first finding. Without it, the test prints the finding and passes.
A finding in a child process that decodes an image ends only that process, as a corrupt image does.
`afterglow-mux` writes a line about it on its standard error, and `aglow` reports that the decoder crashed.
A test that expects a refusal of the image can still pass. `Image.ImageDecode.*` runs the decoders on corrupt files in the test process, where a finding fails the test.

| Leaks that the leak run does not report | Reason |
| --- | --- |
| The tests with the label `gpu` | The Vulkan driver frees its last memory in code that it unloads before the check. The stacks have no names. |
| The Vulkan loader, libdbus and the default input devices of Qt | They keep their memory until the process ends. `tools/sanitizers/lsan.supp` lists them, with the reason for each. |

Keep the default unwinder for the leak run. The full unwinder (`fast_unwind_on_malloc=0`) makes the tests that measure time fail.

## Native test dependencies

The native window suite, the shader scene tests, the project and navigator tests and the native benchmark use a nested KWin desktop.
They need this software:

- `kwin_wayland`.
- An X11 or XWayland display, libX11 and `xprop`.
- `dbus-run-session`.
- ImageMagick `import`, for the tests that take screenshots.
- `kitten`, for the `Kitty*` drag-and-drop tests and the `KittenClipboard*` tests of the native window suite. Without it, those tests skip.

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

The runner creates a nested KWin desktop on a private session bus.
It keeps your `HOME` and `XDG_RUNTIME_DIR`. For this reason, the tests set `Terminal.shell` to `/bin/sh`, which reads none of your startup files.
The test binary runs only in a runner. Without one, it stops with exit status 2.
See [native test dependencies](#native-test-dependencies) for the required software.
It uses the Basic control style and generic Qt platform theme.

| Option | Default | Function |
| --- | --- | --- |
| `--filter` | All tests | A GTest filter, for example `NativeWindow.AglowDragAndDropClient` |
| `--timeout` | 1800 | Seconds for the whole run. The runner stops the suite at the limit and fails. The whole suite takes about 15 minutes. |

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
python3 tools/conformance/test_tuifuzz.py build/tools/conformance/afterglow_tuifuzz
build/tools/conformance/afterglow_screen_checks_tests
```

`test_tuifuzz.py` reports Skipped when `bwrap` is not installed.

### kitten icat

`tools/conformance/kitten_icat.py` runs `kitten icat` of Kitty against the terminal core.
CTest runs it as `afterglow_kitten_icat`, and reports Skipped when `kitten` is not installed.

```sh
python3 tools/conformance/kitten_icat.py build/tools/conformance/afterglow_headless build/tools/render/afterglow_renderpng
```

`kitten icat` runs in `afterglow_headless`, which answers its queries.
With `--graphics FILE`, `afterglow_headless` writes the images, the placements and the Unicode placeholder cells of the terminal to `FILE`.
It writes the file each time the program stops writing output, and at the end.
`afterglow_headless` decodes only the PNG files that tests make: 8 bits for each channel, RGB or RGBA, not interlaced.

The script checks each transfer mode, and that the terminal deletes each temporary file and shared memory object that `kitten` makes.
It also runs `kitten icat` in a private tmux server, when tmux 3.3 or later is installed.
With the second argument, it draws the output of `kitten icat` with `afterglow_renderpng`.
That part needs a Wayland display for Vulkan. Without one, the script skips it.

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

### Character widths with ucs-detect

[ucs-detect](https://github.com/jquast/ucs-detect) measures how many cells a terminal gives to wide characters, emoji sequences, variation selectors and the grapheme clusters of 85 languages.
It reads the cursor position after each one, and compares it with the width that wcwidth gives.
The ucs-detect project publishes the results of other terminals.

The script runs ucs-detect in `afterglow_headless` two times: with grapheme clustering (mode 2027) on and off.
ucs-detect turns the mode on by itself around its tables of characters, so the mode changes only the language results.

1. Make a Python virtual environment in a directory of your choice, and install ucs-detect and its dependencies into it:

   ```sh
   python3 -m venv /path/to/venv
   /path/to/venv/bin/pip install /path/to/ucs-detect
   ```

2. Run the script with a build that has developer tools:

   ```sh
   tools/conformance/ucs-detect.sh build /path/to/venv/bin/ucs-detect
   ```

| Result | Script |
| --- | --- |
| More errors than `tools/conformance/ucs-detect-expected.txt` gives | Prints "worse" and exits with status 1. |
| Fewer errors | Prints "better". Lower the number in the file. |
| A category or a language that the file does not name | Expects no error. |

A third argument keeps the JSON results of the two runs in that directory.
The expected numbers are for ucs-detect 2.3.9 with wcwidth 0.9.4 (Unicode 18.0.0).
Another version can test other characters: run the script, and examine each change before you change the file.
Afterglow joins characters into grapheme clusters as UAX #29 says, and gives a cluster the widths that glibc's `wcwidth` gives its characters, within two cells.
A difference that comes from this rule goes into the file with its reason.

### Fuzzing

The fuzz targets need clang for C and C++, and a separate build directory.
The targets compile these parts with the sanitizers:

- the terminal core and the protocol library, with zlib-ng and xxHash
- the image decoders that `aglow` and `afterglow-mux` share, with libpng, libjpeg-turbo and libwebp
- the codecs of the attach stream of `afterglow-mux`

Build only the fuzz targets in a fuzz build directory.
The commands below turn off the tests and build the four targets by name.
A fuzz build does not build `aglow`.
`aglow` links zlib-ng and the image codecs, which the fuzz build instruments, and a static program cannot link the sanitizer runtimes.

| Target | Input |
| --- | --- |
| `afterglow_vtfuzz` | A byte stream to the terminal core, with control records between reads. One control record sends the rest of the input as a state frame to the reader of a replica terminal. A second replica (the mirror) gets a frame of a writer after each control record. At the end, at the 1 MiB budget, the writer must catch up within 4,096 frames. The mirror must then show the text of the terminal, and the same images with the same frame counts. |
| `afterglow_protofuzz` | The protocol readers for remote input. The first byte selects the reader. |
| `afterglow_imagefuzz` | An image file to the image decoders of `aglow` and `afterglow-mux` (`libs/afterglow-image`). These are its GIF and BMP readers, and libpng, libjpeg-turbo and libwebp. It also stops at a result that breaks what `Decode.h` promises. |
| `afterglow_muxfuzz` | The host calls, the events and the input of the attach stream of `afterglow-mux`. The first byte selects the reader. The state frames of panes are in `afterglow_vtfuzz`. |

Build the targets:

```sh
cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DAFTERGLOW_BUILD_FUZZERS=ON -DBUILD_TESTING=OFF
cmake --build build-fuzz --target afterglow_vtfuzz afterglow_protofuzz afterglow_imagefuzz afterglow_muxfuzz
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
cmake --build build-fuzz --target afterglow_imagefuzz_seeds
./build-fuzz/tools/fuzz/afterglow_imagefuzz_seeds build-fuzz/corpus-image
./build-fuzz/tools/fuzz/afterglow_imagefuzz -max_total_time=300 -max_len=65536 build-fuzz/corpus-image/
```

Run the attach stream target with its own corpus:

```sh
mkdir -p build-fuzz/corpus-mux
./build-fuzz/tools/fuzz/afterglow_muxfuzz -max_total_time=300 -max_len=65536 build-fuzz/corpus-mux/
```

A run passes when the fuzzer stops at the time limit without a crash report.

### Differences with libghostty-vt

`afterglow_vtdiff` sends the same bytes to the terminal core and to [libghostty-vt](https://ghostty.org), and compares the two screens.
It finds a screen that one of the two terminals gets wrong.
The fuzzers find only crashes, and esctest checks a fixed set of xterm cases.

The tool compares these items after each chunk of input:

| Item | Compared values |
| --- | --- |
| Cells | Text, width, colors, attributes and underline style |
| Cursor | Position, pending wrap and visibility |
| Screen | The active screen (main or alternate) |
| Scrollback | The last 200 rows |
| Replies | What each terminal writes back to the program, with `--replies` |

libghostty-vt is not a dependency of Afterglow, and the build downloads nothing.
Build libghostty-vt in a directory of its own:

1. Download `libghostty-vt-1.0.0.tar.gz`, `libghostty-vt-1.0.0-deps.tar.gz` and their `.minisig` files from `https://release.files.ghostty.org/libghostty-vt/1.0.0/`.
2. Check each archive with the public key of Ghostty:

   ```sh
   minisign -V -P RWQlAjJC23149WL2sEpT/l0QKy7hMIFhYdQOFy0Z7z7PbneUgvlsnYcV -m libghostty-vt-1.0.0.tar.gz
   ```

3. Extract both archives in the same directory.
4. Build and install libghostty-vt into a prefix of your choice. Use the Zig version that `build.zig.zon` gives:

   ```sh
   cd libghostty-vt-1.0.0
   zig build --prefix /path/to/libghostty --system ../libghostty-vt-1.0.0-deps -Doptimize=ReleaseFast
   ```

5. Configure Afterglow with the prefix, and build the tool:

   ```sh
   cmake -B build -DAFTERGLOW_BUILD_VTDIFF=ON -DCMAKE_PREFIX_PATH=/path/to/libghostty
   cmake --build build --target afterglow_vtdiff
   ```

Compare files of terminal output, or the files in a directory:

```sh
build/tools/vtdiff/afterglow_vtdiff --expected tools/vtdiff/expected-differences.txt --minimize capture.txt
```

Compare random inputs:

```sh
build/tools/vtdiff/afterglow_vtdiff --expected tools/vtdiff/expected-differences.txt --random 1 --count 400
```

| Option | Default | Function |
| --- | --- | --- |
| `--cols N`, `--rows N` | 80, 24 | The size of both terminals. |
| `--chunk N` | 4096 | The bytes that each terminal reads before a comparison. |
| `--replies` | Off | Also compares the replies. |
| `--expected FILE` | None | The file of expected differences. |
| `--minimize` | Off | Cuts each difference down to the shortest input that still differs in the same kind of way, and prints it. The kinds are the text of a cell, the look of a cell, the cursor, the active screen, the scrollback and the replies. |
| `--random SEED` | None | Makes random inputs from sequences that terminals parse, from seed `SEED`. |
| `--count N` | 1 | The number of random inputs. |
| `--length N` | 512 | The sequences in each random input. |

A line of `tools/vtdiff/expected-differences.txt` is an input name or a `/REGEX/`, then the reason.
A pattern matches the shortest input that `--minimize` prints, with bytes outside ASCII as `\xHH`.
A second `/REGEX/` after the first one also matches the difference of that input, as the run prints it, for example `cell 1,0: "" w1 / "" w1 bg3`. Write a `/` in a pattern as `\/`.
The run fails on a difference that is not expected, and on a named input that no longer differs.

Examine each new difference before you add it to the file:

1. Find what xterm and Kitty do for the input.
2. If Afterglow is wrong, fix Afterglow and add a regression test.
3. If libghostty-vt is wrong, or the terminals disagree with reason, add a pattern for the class of input. Write the reason and the terminal that Afterglow follows.
4. Do not change Afterglow only so that it agrees with libghostty-vt.

### TUI fuzzing

`afterglow_tuifuzz` runs a full-screen program in a pseudo-terminal against the terminal core, and sends it random input.
Real programs write the screens that users see.
The tool checks the screen after each step:

| Check | Fails when |
| --- | --- |
| Size | A screen has a size other than the terminal's. |
| Cursor | The cursor is outside its screen. |
| Rows | A row of the screen does not have one cell for each column, or a row of the scrollback has more. |
| Wide characters | A wide character has no tail cell, or a tail cell has no wide character. |
| Sized text (OSC 66) | A block of sized text lost a cell, or a cell of a block has no first cell. |

A step is one of these actions: text, a key in the encoding that the program asked for (Kitty keyboard protocol included), Ctrl and a letter, mouse reports while the program tracks the mouse, a paste, a focus change, a resize, a scroll of the scrollback, or a selection and a search over it.
After the action, the tool reads the output of the program.
It stops when the program is quiet for 30 milliseconds, or after 400 milliseconds.
When the program exits, the tool starts it again in the same terminal.

The program runs in a [bubblewrap](https://github.com/containers/bubblewrap) sandbox, because random keys can make a program run shell commands, write files or stop processes.
The tool does not run without `bwrap`.

| Item | In the sandbox |
| --- | --- |
| Root file system | Read-only |
| `/home`, `/run`, `/tmp` | Empty and private. The sockets of your sessions are not there. |
| Home directory | `/tmp/home`, from `--home DIR`, or an empty directory that the run deletes |
| Processes, network, IPC | Namespaces of their own. A program sees and stops only the processes of the sandbox. |
| Environment | Cleared, then `HOME`, `PATH`, `TERM=xterm-256color`, `COLORTERM`, `LANG=C.UTF-8`, `SHELL=/bin/sh`, `XDG_RUNTIME_DIR` and `TMUX_TMPDIR` (both `/tmp/run`, a directory of the sandbox) and the `--env` values |

Fuzz the installed programs, 500 steps each:

```sh
tools/conformance/tuifuzz.sh build 500
```

The script runs vim, nvim, less, nano, tmux, btop and top, and skips a program that is not installed.
It gives each program a home with a sample text file, and uses the restricted mode of the program where it has one.
The arguments after the build directory are the steps, the seed and a directory for recordings.

Fuzz one program:

```sh
build/tools/conformance/afterglow_tuifuzz --seed 7 --steps 2000 --env LESSSECURE=1 -- less /etc/os-release
```

| Option | Default | Function |
| --- | --- | --- |
| `--cols N`, `--rows N` | 80, 24 | The size at the start. |
| `--seed N` | 1 | The seed of the input. The same seed sends the same input, but the timing of a program can change its output. |
| `--steps N` | 1000 | The number of steps. |
| `--seconds N` | None | Stops the run after this time. |
| `--home DIR` | A new empty directory | The home directory of the program. |
| `--env NAME=VALUE` | None | An environment variable for the program. |
| `--record DIR` | None | Writes the output of the program to `DIR/SEED-NNNN-COLSxROWS.vt`, a new file at each resize. |

A failure prints the program, the seed, the step, the last action and the screen, and the tool exits with status 1.
The tool exits with status 2 when the program does not start.
A program that crashes is reported, and the run continues.

The recordings are input for the other tools:

1. Compare them with libghostty-vt. `afterglow_vtdiff` takes the size of each file from its name:

   ```sh
   build/tools/vtdiff/afterglow_vtdiff --expected tools/vtdiff/expected-differences.txt recordings/vim
   ```

2. Copy them into the corpus directory of `afterglow_vtfuzz`.

Run the fuzzer on the [sanitizer build](#sanitizer-build) before a pull request that changes the parser or the screen:

1. Turn on the tools in the sanitizer build, and build the fuzzer with two jobs:

   ```sh
   cmake -B build-debug -DAFTERGLOW_BUILD_TOOLS=ON
   systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 cmake --build build-debug -j2 --target afterglow_tuifuzz
   ```

2. Run the programs:

   ```sh
   ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=suppressions=$PWD/tools/sanitizers/lsan.supp \
       UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 tools/conformance/tuifuzz.sh build-debug 2000
   ```

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
| `--samples` | 100 input acknowledgments | 1 to 1000 |
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
With `AFTERGLOW_NATIVE_TESTS=ON` and `AFTERGLOW_BUILD_AGLOW=ON`, CTest also runs the `--kitty` test as `afterglow_transfer_kitty`.

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
ctest --test-dir build --output-on-failure -R '^(ControlSocketTest|CtlBinaryTest|CtlOverlayTest|CtlScreenshotTest|CommandSocketTest|RunBinaryTest|AfterglowRunWithoutAglowTest|RemoteCommandsTest|Proto\.RunClient|Proto\.RunInput|Proto\.RunRemote|Proto\.Socket|Proto\.Remote|Proto\.Ipc)\.|RemoteSocket|aglow_run_pty|aglow_tmux_pty'
```

| Tests | Binary | What they run |
| --- | --- | --- |
| `ControlSocketTest.*` | `afterglow_control_tests` | The `control` target: calls and errors, `overlay` and its `overlayInput` parts, and the refusals of `screenshot`. The socket lookup, stale sockets and merged events. |
| `CtlBinaryTest.*`, `CtlOverlayTest.*` and `CtlScreenshotTest.*` | `afterglow_command_tests` | `afterglow ctl` against a socket that the test makes: usage errors, `afterglow ctl overlay` with its options and standard input, and `afterglow ctl screenshot` with a `FILE`, `-`, a pipe, a FIFO that a program reads, a socket, a terminal and a directory that takes no new file |
| `CommandSocketTest.*` | `afterglow_command_tests` | The `commands` target of the socket: the list, runs, standard input, flow control, cancel and the limits |
| `RunBinaryTest.*` | `afterglow_command_tests` | `aglow run` against a socket that the test makes. They check the exit status, the output, standard input, the pane, signals and the socket lookup. |
| `AfterglowRunWithoutAglowTest.*` | `afterglow_command_tests` | `afterglow run` in a build without `aglow`. The test checks the error and exit status 127. |
| `Proto.RunClient.*` and `Proto.RunInput.*` | `afterglow_proto_tests` | The client of `aglow run` against a server that the test plays, and the parts of standard input that it sends |
| `Proto.RunRemote.*` | `afterglow_proto_tests` | How the client of `aglow run` uses the answer of the terminal to OSC 24569, and its messages when there is no socket |
| `Proto.Socket.*` | `afterglow_proto_tests` | The socket lookup of `afterglow ctl` and `aglow run`, and the sockets of the mux daemon: their order and the name of the state socket |
| `Proto.Remote.*` | `afterglow_proto_tests` | OSC 24569, the remote directory and `aglow remote-prune` |
| `RemoteCommandsTest.*` | `afterglow_remote_tests` | The sockets of remote commands, the probe of `afterglow ssh` with a fake `ssh`, and an acceptance test with `sshd` |
| `Proto.Ipc.*` | `afterglow_proto_tests` | The phosphor-ipc client of the protocol library |
| `Extensions.*RemoteSocket*` | `afterglow_vt_tests` | The answer of the pane to OSC 24569: the path or the reason, and the order of the answer before DA1 |
| `aglow_run_pty` and `aglow_tmux_pty` | Python scripts | The OSC 24569 query of `aglow run` in a pseudo-terminal, also in a private tmux server. See [aglow tests](#aglow-tests) |

`NativeWindow.AnOverlaysProgramGetsTheRemoteSocketOfItsHost` in the [native window suite](#native-window-tests) checks the answer to OSC 24569 in an overlay.
`NativeWindow.ScreenshotsShowThePaneItsTabAndItsWindow` checks the images of `screenshot` and its refusals in a window.

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

## Mux daemon tests

Run the tests of the [mux daemon](MUX.md), of `afterglow mux` and of the server side of its protocol:

```sh
ctest --test-dir build --output-on-failure -R '^(Proto\.(IpcServer|Socket)|Mux|MuxCommandTest|StateSync|Image|WindowImageDecode)\.' -j4
```

| Tests | Binary | What they run |
| --- | --- | --- |
| `Proto.IpcServer.*` | `afterglow_proto_tests` | The server side of the phosphor-ipc wire format that `afterglow-mux` uses: requests, errors, the listener and long lines |
| `Proto.Socket.*` | `afterglow_proto_tests` | The socket lookup of `afterglow ctl` and `aglow run`, and the sockets of the mux daemon: their order and the name of the state socket |
| `Mux.MuxProtocol.*` | `afterglow_mux_tests` | The values of the calls of `afterglow-mux`, and session names. The keys and fields that belong to the protocol version. The bytes of the attach stream and of each host call, and the frames and calls that its decoders refuse. |
| `Mux.MuxDaemon.*` | `afterglow_mux_tests` | `afterglow-mux` with a runtime directory and a state directory of its own. The cases are sessions, the environment of a pane, the answers to queries, the refused control calls, and when the daemon exits. |
| `Mux.MuxAttach.*` | `afterglow_mux_tests` | A window of a session, played without Qt. The cases are listed below the table. |
| `Mux.MuxImageDecoder.*` | `afterglow_mux_tests` | The image decoder of `afterglow-mux`. It decodes the pixels of a PNG with alpha, a JPEG, a WebP and a BMP. It decodes the first frame of a GIF and of a WebP animation. It refuses a format other than PNG for Kitty graphics and formats that it does not read. It refuses a cut PNG, Apple's CgBI PNG, a side of 10,001 pixels and more than 320 MiB of pixels. A side of 10,000 pixels decodes. A server that is missing does not start again for a minute, and neither do two in a row that end before they answer. A server that ended after it answered starts again, and so does a server killed before its first image. One that did not answer by its deadline starts again too. A decode ends at its deadline, and when its pane closes, also while it waits for another pane's decode. A pane that closes while its request waits leaves the same server to the other panes, and the process of its image ends. A decoding process has no new privileges, the filter of its system calls and limits on its memory, its CPU time and its core file. Four panes decode at once. The server runs only as the child of its daemon. |
| `Mux.MuxPane.*` | `afterglow_mux_tests` | A pane of `afterglow-mux` while its program's image decodes. The daemon's calls return at once, and a resize that waited reaches the next frame, for which the pane wakes the daemon. A change of visibility waits for the decode. Input waits in the pane behind a resize and a clear, and reaches the program after them. The pane keeps no more input than half of the PTY's queue, with what the PTY still holds. A pane that closes ends its decode at once. The tests stop the decoder's server (SIGSTOP) to make a decode wait. They skip when the kernel does not list the children of a process. |
| `Image.ImageDecode.*` | `afterglow_image_tests` | The image decoders of `libs/afterglow-image` in the test process. PNG of each kind: RGBA, RGB, gray and gray with alpha, 2 bits, 16 bits, a palette, transparency, interlace and a gAMA chunk. JPEG: color, gray, progressive, Adobe CMYK, warnings, a fatal error after a warning, 12 bits, the scan limit and a side past the limit. WebP: lossless, lossy, with alpha, and animated, with the frames before damage and the end of a long animation. GIF: the frames and delays of an animation, the transparent canvas, the blocks and the cut files. BMP of each depth and header. The format from the signature, the size from each header, and the files that they refuse. |
| `WindowImageDecode.*` | `afterglow_imagedecode_tests` | The decoder of a pane outside a session (Qt), with the files of `Image.ImageDecode.*`. It applies the gAMA chunk of a PNG, as Kitty does. It counts the gAMA, sRGB and cICP chunks as libpng does. It reads a top-down BMP. |
| `StateSync.*` | `afterglow_vt_tests` | The state codec of the mux (`afterglow.state.v1`). A replica terminal that gets only its frames must equal the terminal that parsed the output. The cases are listed below the table. |
| `MuxCommandTest.*` | `afterglow_mux_command_tests` | The commands of `afterglow mux`. The cases are listed below the table. |

The `StateSync.*` tests cover these cases:

- Text, styles, links, wide and combined characters, and the long text of a sizing block, also after a resize flattens it
- Scrolling in small frames, a replica that starts late, a replica that keeps less history, and a view scrolled back into the history
- The alternate screen, a resize, cleared and dropped history, and 60 runs of random output
- A history row larger than a frame still goes, and a screen larger than a frame goes in several frames
- Rows that a frame left out go first in the next frame
- The link definitions of history rows count in the size of a frame
- A damaged frame and a link without a URI, which the reader refuses without a crash
- After a refused frame, the reader takes the first frame of a writer that starts over
- Each synchronized update, with its own count
- The marks and regions of shell integration (OSC 133): the last output, the text of each region, and whether the prompt takes clicks
- The same marks and regions after they scroll into the history
- Program status and the shell contexts, with one call to the replica's host for each frame that changes them
- Extra cursors and their colors
- Region scrolls: the replica's host hears of each, with the screen that scrolled. A new replica gets none.
- Two scrolls of one region in one direction, one right after the other, come as one
- Exactly 256 region scrolls in one frame all go.
- Past 256, the rows from the top of the highest region that scrolled to the bottom of the lowest leave their region. The history stays.
- A scroll within margins, an iTerm2 remote host, an exit code of two varint bytes, and a mark one past the last column
- While the history comes over many frames, each row of the replica has the writer's id
- A new layout of the writer, also at the same size, numbers the replica's rows anew. A change of height keeps them.
- Rows of many links: no frame passes its budget by more than the bytes of one row
- A writer for a replica with less history sends no older rows
- Images of both screens with their placements: a relative placement, a virtual placement, a Sixel, and a deleted image with its placements
- An image larger than a frame comes in parts and shows only once all of it came. An image of a lower id waits for it. An image deleted before all of it came lets the next one come.
- An animation runs on the replica's clock: output and a new frame keep its step. A command that stops it, jumps to a frame or plays it again reaches the replica, also when it changes no value of the writer's store.
- A deleted frame keeps the frame that shows, and a new image under an id starts its animation over
- An image that a program sends again faster than its pixels go still shows, and the replica keeps only the frames of the last version
- Many images, a command to each of them, and the frames that go when they go, all in state frames within the budget and one item more
- A transfer that ends drops the frame that the replica builds, also one that another image still holds
- A clear that replaces an image ends its transfer
- An image that a frame deletion completes shows at once, and the version with the deleted frame never replaces it, also after a new frame
- A version that shows gives its place to the store's image once that image is whole, and the replica keeps only the frames of that image
- A frame deletion that leaves an image whole while its transfer runs ends that transfer at once
- The frames that only a version that showed held go with its record, also when nothing else waits
- A reader takes a writer that starts over: the images of the old writer go
- Bad image sections, each to a reader of its own, which refuses them
- The random runs place, animate and delete images and sixels too, also images larger than a state frame, between state frames
- The prompt redraw after a resize (OSC 133 `redraw=1`)
- Shell mark, program status, shell context, extra cursor and region scroll sections with bad values, which the reader refuses
- A frame that fails after its status and contexts tells the replica's host nothing, and bad modes leave the modes and the screen
- The reader tells a frame of output from one of history that catches up
- The rows of either screen that wait for a later frame count as rows that wait, and history and image pixels do not

The `Mux.MuxAttach.*` tests cover these cases:

- The attach call, its keys and its socket, which only the caller may connect to, and which goes after the connect wait
- The screen, the input, the size of the window, and the channel of a pane
- Reset and Clear, which change the terminal of the session
- A takeover, a detach, also one before the window connects, and a window that leaves
- The last output of a program that exits, before its exit, and a kill
- A window that reads slowly: fewer frames, and no lost output
- Input that a full PTY refuses: it waits and arrives whole, and a Kill goes past it
- The attach sockets that an earlier daemon left, and calls that the daemon refuses
- The calls of the pane's terminal to its host reach the window: a bell, an OSC 52 write and a file transfer, in their order
- With a window, the daemon writes no refusal of a file transfer or a drag and drop query to the program
- A frame that waits for a decode holds the calls of the pane behind it: a bell comes after the frame with its text
- A clipboard read waits for the window's answer, and the daemon gives none
- The answer to a query (DA1) comes after the drag and drop query before it
- A request larger than the window's `eventQueue` goes in parts when nothing else waits, and the requests after it follow it
- A request that does not fit the queue gets the answer of a session that no window shows, once
- A request that waits when the window detaches, in the queue of the session or of the pane, gets that answer once
- A request whose first part went goes whole when the session ends
- An `eventQueue` or a `history` out of range, and an unknown option of the attach call, are refused
- A window that keeps no history gets no history rows, and a window with the default gets them
- The window says whether you can see the pane, and mode 2033 reports each change
- When the window detaches, the program hears that you cannot see the pane. The next window, and a window that takes the session over, report whether you can see it.
- The `paneDirectory` call: the directory of the shell, a directory that was removed, then the program in the foreground, and the calls that the daemon refuses
- The frames carry the region scrolls since the frame before, and the first frame of a window carries none of a time when no window showed the pane
- A PNG (Kitty graphics `f=100`), an iTerm2 JPEG and an iTerm2 WebP from the session's shell reach the window with their pixels
- A JPEG sent with `f=100` does not reach the window
- An image that the program prints while no window shows the session reaches the next window

The `MuxCommandTest.*` tests cover these cases, with the daemon that `afterglow mux new` starts in a session of its own:

- `afterglow mux new`, `ls` and `kill`
- The usage errors of every command, and `detach` without a daemon
- The refusals of `attach` and `new` without a Wayland display, and of `new` with another `QT_QPA_PLATFORM`
- The start errors of the daemon, a socket that another daemon holds, and a daemon of another protocol version
- A start of the daemon while `afterglow mux` inherited `SIGCHLD` as ignored
- The largest and smallest values of the settings that a session takes

These tests in the [native window suite](#native-window-tests) show a session in a real window, with a daemon of the test:

| Test | Function |
| --- | --- |
| `NativeWindow.AMuxSessionShowsInAWindowAndEndsWithIt` | The screen, typed input, the size of the window, and the kill of `Mux.onClose` |
| `NativeWindow.AMuxSessionWhoseProgramExitsClosesItsPane` | The last output and the exit status of the program |
| `NativeWindow.AWindowThatLosesItsSessionLeavesItRunning` | A detach and a takeover close the pane and kill nothing |
| `NativeWindow.AMuxSessionCanOutliveItsWindow` | `Mux.onClose` detach |
| `NativeWindow.AMuxSessionsBellsUserVarsAndClipboardWritesReachTheWindow` | A bell, a user variable and an OSC 52 write of the session's shell reach the window and its clipboard |
| `NativeWindow.AMuxSessionsImageShowsInTheWindow` | A PNG that the session's shell prints with Kitty graphics, which the daemon decodes, shows in a screenshot of the pane at its own size |
| `NativeWindow.AMuxSessionsAnimationRunsInTheWindow` | An animated image of a session's program runs on the window's clock without output, and stops on the frame that shows for the program's command. It does not move while its tab is hidden behind another tab, and runs again once that tab shows. |
| `NativeWindow.AMuxSessionsMarksStatusAndDirectoryReachTheWindow` | The last output in the pager, and the regions of shell integration. The status of a program, with the `Pane::statusChanged` signal. A shell context. A new tab starts in the directory that the daemon gets from the system, because the shell reports none. |
| `NativeWindow.ASessionPaneTellsItsProgramWhetherItCanBeSeen` | Mode 2033 reports at the attach, when another tab shows, and when the pane shows again |
| `NativeWindow.AnAttachShowsTheSessionsStateAndNotifiesNothingOld` | A window that attaches gets the history, the last output and the prompts of the session. It shows no notification for a status from before the attach. |
| `NativeWindow.ASelectionInASessionPaneFollowsTheScrollsOfItsProgram` | A selection moves with its text when the program deletes a line, and goes when a region scroll pushes it out. The extra cursors of the program show. |
| `NativeWindow.AMuxSessionsReadsAndCommandsReachTheWindowAndBack` | An OSC 52 read gets the clipboard after the prompt. The commands of shell integration come in order, with the time that the daemon measured. |
| `NativeWindow.AProtocolWaitOfASessionPaneReadsItsStream` | The answer wait of a drag reads the session's stream, and ends when the command comes |
| `NativeWindow.ALaunchOfASessionPassesItsEventQueue` | A launch of a session passes `Mux.eventQueue` of the configuration to the daemon, in bytes |
| `NativeWindow.AReadOfASessionThatOutlivesItsWindowGetsTheDenial` | A clipboard read that waits for its prompt gets the denial when the window closes and the session keeps running |
| `NativeWindow.MuxOnCloseAppliesToAnOpenSessionPane` | A change of `Mux.onClose` reaches an open pane |
| `NativeWindow.ALaunchOfASessionThatThisProcessShowsOpensNoOtherWindow` | A launch of a session that a window shows brings that window to the front and shows the session pane. A launch of an unknown session fails with the reason. |
| `NativeWindow.ALargePasteReachesTheProgramOfASessionWhole` | Input larger than a frame goes in parts, in order |

Not every test starts a daemon. A test that starts one gives it an `XDG_RUNTIME_DIR`, an `XDG_STATE_HOME` and a `HOME` of the test.
The `Mux.*` tests and the native tests above do this with `mux/tests/TestDaemon.h`, and the `MuxCommandTest.*` tests in their `SetUp`.
A native test that shows a session also gives the window the sockets of its daemon. A launch with `--mux-attach` uses the sockets of the environment, so a test of it must set these directories first.
Thus a test daemon never reaches the mux daemon of the user, and never starts in a systemd user unit.
The tests get the name of the state socket from `AfterglowProto::muxStateSocketName`, which the daemon uses too (see [sockets](MUX.md#sockets)).
At its end, each `MuxCommandTest.*` test kills the processes that have its `XDG_RUNTIME_DIR`.
Some `MuxCommandTest.*` tests put an `afterglow` with a script as its daemon in `build/afterglow/muxcommand-PID-*`.
The test removes the directory at its end. When a timeout kills the test first, the next `MuxCommandTest.*` test removes it.
Some of the tests need `python3`. Without it, they report Skipped.
`Mux.MuxPane.*` and some `Mux.MuxImageDecoder.*` tests need a kernel that lists the children of a process (`/proc/PID/task/TID/children`). Without it, they report Skipped.
Some image tests write a script to the temporary directory and run it. When that directory does not run programs (`noexec`), they report Skipped.
`Mux.MuxDaemon.*` also skips its test of open files when the hard limit of open files is 256 or less.
The image tests fail on a kernel without seccomp filters, and on a processor that the filter does not know. The daemon does not decode images there (see [MUX.md](MUX.md)).
With `-j4`, the tests take about 15 seconds. `Mux.MuxDaemon.ADaemonThatNobodyReachesEnds` waits out the start wait of the daemon, which is 10 seconds.
Without `-j`, the tests take about 50 seconds.

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
| `aglow_only_build` | Configures `aglow` alone (`AFTERGLOW_AGLOW_ONLY`), with `find_package(Qt6)` disabled and no Phosphor directory. Builds it and installs it into the build directory. Checks that the installation holds only `bin/aglow`, its license notices and its completions. Uses the compilers, the build type and the flags of the build, also the flags of each build type. Has the label `slow` | A full build, and the source archives of `cmake/AfterglowDependencies.cmake` or a network connection |
| `aglow_clipboard_pty` | Plays a terminal for `aglow clipboard` in a pseudo-terminal | Python 3.9 or later |
| `aglow_run_pty` | Plays a terminal and a terminal socket for the OSC 24569 query of `aglow run`. Checks when `aglow run` does not ask, the modes of the terminal after signals and `Ctrl+C`, and what `aglow remote-prune` removes | Python 3.9 or later |
| `aglow_tmux_pty` | Runs `aglow icat` and `aglow run` in a private tmux server (`tmux -L`, its own configuration and socket directory) in a pseudo-terminal, with `allow-passthrough` on and off | Python 3.9 or later and tmux 3.3 or later. With no tmux, or a tmux older than 3.3, it reports Skipped |
| `aglow_clipboard_cases` | Checks the arguments that `aglow clipboard` refuses, a new output file, and the timeout of an OSC 52 read | Python 3.9 or later, and a kernel that shows a process that waits in `open()` |
| `aglow_icat_decode` | Sends crafted images to `aglow icat` and checks the pixels and keys that it sends, or its error. The images are PNG, JPEG, GIF, WebP and BMP files, also animations and damaged files. The cases include delays, `--loop 0`, a side of 10,001 pixels and EXIF turns. They also include a FIFO and a closed output | Python 3.9 or later, and a user that cannot read a directory of mode 0 |
| `aglow_icat_render` | Draws the output of `aglow icat` with `afterglow_renderpng` | Python 3.9 or later, `AFTERGLOW_BUILD_TOOLS`, a Wayland display and a Vulkan device |

Without `readelf`, the configuration prints a message and leaves out `aglow_static`.
`aglow_only_build` compiles zlib-ng, xxHash and the image codecs again, which takes some minutes.
Run the suite without it:

```sh
ctest --test-dir build --output-on-failure -LE slow
```

A [build of aglow alone](BUILD.md#build-aglow-alone) has the `Proto.*` and `Image.*` tests and these tests of the table:

- `aglow_version`, `aglow_run_usage`, `aglow_no_command`, `aglow_unknown_command` and `aglow_help_unknown_command`
- The three `aglow_remote_prune_*` tests, and `Aglow.*`
- `aglow_static`, `aglow_clipboard_pty`, `aglow_run_pty`, `aglow_tmux_pty`, `aglow_clipboard_cases` and `aglow_icat_decode`

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
