---
name: ag-build
description: "Configure, build and test Afterglow, and decide when a build is green. Covers the build directories, the Phosphor libraries built from the PlasmaZones checkout, warnings, the CTest suite, and which tests need a GPU or a nested KWin. Use when: build, rebuild, run the tests, run ctest, does it compile, is it green, before committing."
---

<!--
SPDX-FileCopyrightText: 2026 fuddlesworth
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Build and test Afterglow

Run every command from the repository root. `docs/BUILD.md` and `docs/TESTING.md` are the user-facing versions of
this procedure. This skill adds what a session gets wrong.

## The build directories

Check what a directory is before you trust a result from it:

```sh
grep -E "^(BUILD_TESTING|CMAKE_BUILD_TYPE|AFTERGLOW_[A-Z_]+):" build*/CMakeCache.txt
```

| Directory | Type | Tools | Use |
| --- | --- | --- | --- |
| `build` | RelWithDebInfo (`dev` preset) | ON | Day-to-day work. All the commands below use it. |
| `build-release` | Release | ON | Performance numbers and release checks |
| `build-prof` | RelWithDebInfo | ON | Profiling with `perf` |
| `build-fuzz` | RelWithDebInfo, clang for C and C++, `AFTERGLOW_BUILD_FUZZERS=ON`, `BUILD_TESTING=OFF` | OFF | The libFuzzer targets `afterglow_vtfuzz`, `afterglow_protofuzz`, `afterglow_imagefuzz` and `afterglow_muxfuzz`, and the seed writer `afterglow_imagefuzz_seeds`, only. Build them by name: the other binaries are not supported there (zlib-ng and the image codecs are instrumented in place). The fuzz option turns `aglow` off. Make it when you fuzz, with the commands in `docs/TESTING.md`. |
| `build-debug` | Debug with ASan and UBSan at `-O1` (`debug` preset) | — | Make it when you look for memory errors, and before a PR that changes a parser, a protocol or a pane's memory. Build it with `-j2`: at `-j4` the compiles hit the 6 GB cap. `docs/TESTING.md` "Sanitizer build" has the three runs (errors, leaks without `-L gpu`, native). `aglow` links dynamically there, and `aglow_static` is left out. |

## aglow

`AFTERGLOW_AGLOW_ONLY=ON` builds only `libs/afterglow-proto`, `libs/afterglow-image` and `tools/aglow`: no Qt, no
Phosphor checkout, and an install of `bin/aglow`, its completions and its notices in `share/licenses/aglow`.
`docs/BUILD.md` has the procedure.

`AFTERGLOW_BUILD_AGLOW` is on by default. `aglow` links statically, so the configuration needs `libc.a` and
`libstdc++.a` (Fedora: `glibc-static` and `libstdc++-static`). Without them, the configuration stops with a message
that names the packages. Do not install them: tell the user, or configure with `-DAFTERGLOW_BUILD_AGLOW=OFF`. Then
the `aglow` tests are left out. The `aglow dnd` tests of `afterglow_dnd_tests` and of the native suite skip, and so
do the `RunBinaryTest.*` tests of `afterglow_command_tests` and the `sshd` test of `afterglow_remote_tests`
(`RemoteCommandsTest.OpenSshForwardsAsTheDesignExpectsAndAglowRunWorksThroughIt`).
`aglow` builds next to `afterglow` (`build/afterglow/aglow`), and building `afterglow` builds it too.

`libs/afterglow-image` builds libpng, libjpeg-turbo and libwebp from their pinned archives into `aglow` and
`aglow-mux`, also with `-DAFTERGLOW_BUILD_AGLOW=OFF`. No image library of the system is used. libjpeg-turbo
builds with its own CMake during the build, in `build/_afterglow_deps/libjpeg-turbo-build`. When that step fails,
its output is in the logs under `build/_afterglow_deps/libjpeg-turbo-steps`. It uses NASM when NASM is installed.
It runs `AFTERGLOW_CODEC_BUILD_JOBS` jobs (2 by default) beside the main build: count them in the memory cap.

`AFTERGLOW_BUILD_TOOLS=ON` builds `afterglow_renderpng`, `afterglow_headless`, `afterglow_vtbench` and
`afterglow_native_bench`. Shader previews, `aglow_icat_render` and `afterglow_headless_runner` need it. The renderer
tests (`Render.*`) do not: `BUILD_TESTING` builds them.

## aglow-mux

`aglow-mux` (`mux/`) is the session daemon of `afterglow mux`. It has no Qt and links `afterglow-vt`,
`afterglow-pty`, `afterglow-proto`, phosphor-channel and `afterglow-image`, which its image decoder uses. It builds
next to `afterglow` (`build/afterglow/aglow-mux`), because
`afterglow mux` runs the daemon from its own directory, and building `afterglow` builds it too. A build of `aglow`
alone (`AFTERGLOW_AGLOW_ONLY`) does not build it. Its tests are `afterglow_mux_tests` (`Mux.*`) and the client
tests of `afterglow mux` in `afterglow_mux_command_tests` (`MuxCommandTest.*`), with the server and socket tests of
`afterglow-proto`: `ctest -R '^(Proto\.(IpcServer|Socket)|Mux|MuxCommandTest)\.' -j4` takes about 10 seconds. Most
of that time is the start wait of `Mux.MuxDaemon.ADaemonThatNobodyReachesEnds`. Without `-j`, the run takes about 30
seconds. `afterglow mux new` without `--detach`, `afterglow mux attach` and `afterglow --mux-attach` also open a window:
the native tests show sessions with a daemon of their own (`mux/tests/TestDaemon.h`).

`afterglow mux` (every command but `--help`) and `aglow-mux` (every run but `--help` and `--version`) reach the
user's mux daemon and its sessions, also from a nested KWin. Run them only with a private `XDG_RUNTIME_DIR` and
`XDG_STATE_HOME` under `/tmp`, as these tests do (they also give a `HOME` of their own), and kill the sessions that
you made. Never stop, restart or mask the user's `aglow-mux.service`, and never remove the files in the user's
two `aglow-mux` directories.

## The Phosphor libraries

`cmake/AfterglowPhosphor.cmake` compiles the Phosphor libraries from a PlasmaZones checkout. The default is
`../PlasmaZones/phosphor-libs`. A build directory can name another checkout with `-DAFTERGLOW_PHOSPHOR_LIBS_DIR`.
Each build directory keeps its own value, and the `grep` at the top of this skill shows it. The state of that
checkout is part of the Afterglow build. When a build fails or changes for no reason in this repository, check the
checkout first:

| Condition | Check | Action |
| --- | --- | --- |
| The checkout is not on `main` or at the commit that `docs/BUILD.md` names, or has uncommitted work. | `git -C <checkout> status -sb` | Tell the user. |
| The checkout does not contain the PlasmaZones commit that `docs/BUILD.md` names. | `git -C <checkout> merge-base --is-ancestor <commit> HEAD` | Tell the user which commit the build needs. |
| The checkout contains that commit, and the build fails in a Phosphor call. | Compare the call with the Phosphor header in the checkout. | If Afterglow calls a newer API, name its commit in `docs/BUILD.md` in the same change. Tell the user that the checkout needs it. If PlasmaZones changed the API after the commit, tell the user. |

- Never edit a checkout that a build directory compiles from. Update one only with the user's approval. On a
  clean `main`, use `git pull --ff-only`. On a detached checkout, use `git checkout <commit>`. A Phosphor change
  goes through a PlasmaZones branch and pull request. Tell the user what Phosphor needs.
- The renderer uses Qt private APIs. After a Qt update, rebuild everything in the capped scope:
  `systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 cmake --build build --clean-first -j6`.
- The pinned archives (Luau and valijson from `PhosphorDependencies.cmake` in PlasmaZones, zlib-ng, xxHash, libpng,
  libjpeg-turbo and libwebp from `cmake/AfterglowDependencies.cmake`) download into the CPM cache. By default that is
  `.cache/cpm` in the PlasmaZones checkout that the build uses, and Afterglow's own `.cache/cpm` when only
  `PHOSPHOR_DEPS_ARCHIVES` is set (`docs/BUILD.md` has the order). A configure without network works once that cache
  holds them. A build of `aglow` alone loads no PlasmaZones module, so its default cache is Afterglow's own
  `.cache/cpm`.

## Build

```sh
systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 \
    cmake --build build -j6 2>&1 | tee <scratchpad>/build.log | tail -5
grep -c "warning:" <scratchpad>/build.log
```

`<scratchpad>` is the session scratchpad directory. Never write logs into the repository.

- Always build and test in the capped scope above. The session runs in the cgroup of the user's Afterglow, and
  when memory runs out, systemd-oomd kills that whole unit with every window of the user. In the scope, the
  kernel stops the build at the cap. Run one build or test run at a time.
- Use `-j6`. The machine has 8 cores, and the user works on it while you build.
- The build uses `-Wall -Wextra -Wpedantic` with no `-Werror`. A build with warnings is not green. The user treats
  a warning as a defect. An incremental build shows warnings only for the files it compiled again.
- `QT_NO_KEYWORDS` is on for the whole project: write `Q_EMIT`, `Q_SIGNALS` and `Q_SLOTS`. `QT_NO_CAST_FROM_ASCII`
  is on for the application targets and `afterglow-render`: a raw `"string"` into a `QString` does not compile
  there. Use `QStringLiteral` or `QLatin1String`, also in tests that link those targets.
- Never install (`cmake --install`, the `install` target), and never use `sudo`. The user installs Afterglow.
  The one exception: `cmake --install` into a temporary directory (`--prefix` or `DESTDIR` under `/tmp`), to check
  the install layout.

## Test

```sh
systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 \
    ctest --test-dir build --output-on-failure -j6 2>&1 | tail -15
```

The suite has about 2,000 tests. All of them must pass. Use `-R PATTERN` while you work, and run the whole suite
before a commit.

- `aglow_only_build` (label `slow`) configures, builds and installs `aglow` alone (`AFTERGLOW_AGLOW_ONLY`) in
  `build/tools/aglow/aglow-only`, with Qt hidden and no Phosphor directory. It compiles zlib-ng and the image codecs
  again and takes some minutes. `-LE slow` leaves it out while you work. The whole suite before a commit includes it.
- The renderer tests (`libs/afterglow-render/tests`), `afterglow/tests/PopupLookTest.cpp` and
  `afterglow/tests/PointerMapTest.cpp` (`afterglow_pointer_map_tests`) need a Wayland display and a working Vulkan
  driver. They skip only on a machine with no Wayland display or no Vulkan. A device that cannot be made fails them.
  They take turns on the GPU (`RESOURCE_LOCK gpu`, also `aglow_icat_render`): each test process makes its own device,
  and several at once ran out of device memory beside the desktop's own use of the GPU. Give a new GPU test the same
  lock.
- The native window tests (`afterglow_native_tests`, the `Shaders/` tests included) are not in CTest by default
  (`AFTERGLOW_NATIVE_TESTS=OFF`).
  They need a nested KWin. Use the `ag-verify-live` skill to run them.
- CTest runs some Python tests: `completion_test.py` (`afterglow_shell_completion`), the relays of
  `kitty_transfer_interop.py`, `tools/generate-reference.py --check`, the scripts in `tools/aglow/tests`, and
  `test_headless.py` (`afterglow_headless_runner`), `test_tuifuzz.py` and `kitten_icat.py` in `tools/conformance`. With
  `AFTERGLOW_NATIVE_TESTS=ON` it also runs `native_window_acceptance.py` and `kitty_transfer_interop.py --kitty`. The
  other Python suites in `afterglow/tests/*.py` are separate: `desktop_acceptance.py`, `scene_acceptance.py`,
  `shader_parameters_smoke.py`, `project_smoke.py` and the two navigator tests. Run the ones that cover the area that
  you changed (`docs/TESTING.md` lists them).

## Generated files

| File | Command |
| --- | --- |
| `docs/CONFIGURATION.md` | `python3 tools/generate-reference.py build/afterglow/afterglow` after a change to a setting, its default or its description |
| `libs/afterglow-vt/src/UnicodeTables.inc` | `python3 -I tools/gen_unicode_tables.py <ucd-dir> libs/afterglow-vt/src/UnicodeTables.inc` |

The guard hook refuses a direct edit of these files.

## Subagents never build

Reviewer and audit subagents read and search. They do not run `cmake`, `ctest` or the test binaries. Parallel
builds overload the machine and give results that nobody can attribute. The main session builds and tests once,
serially.

## Green does not mean it works

A passing suite does not prove that a visible change looks right or that an animation settles. For anything that
renders, check it with `afterglow_renderpng` (the `ag-shader-pack` skill) or in a nested KWin (`ag-verify-live`).
The commit message says what you ran.
