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
| `build-prof` | RelWithDebInfo | ON | Profiling (Tracy and perf) |
| `build-fuzz` | RelWithDebInfo, clang for C and C++, `AFTERGLOW_BUILD_FUZZERS=ON`, `BUILD_TESTING=OFF` | OFF | The libFuzzer targets `afterglow_vtfuzz`, `afterglow_protofuzz` and `afterglow_imagefuzz` only. Build them by name: the other binaries are not supported there (zlib-ng is instrumented in place). The fuzz option turns `aglow` off. Make it when you fuzz, with the commands in `docs/TESTING.md`. |
| `build-debug` | Debug with ASan and UBSan (`debug` preset) | — | Make it when you look for memory errors. `aglow` links dynamically there, and `aglow_static` is left out. |

## aglow

`AFTERGLOW_AGLOW_ONLY=ON` builds only `libs/afterglow-proto` and `tools/aglow`: no Qt, no Phosphor checkout, and an
install of `bin/aglow`, its completions and its notices in `share/licenses/aglow`. `docs/BUILD.md` has the procedure.

`AFTERGLOW_BUILD_AGLOW` is on by default. `aglow` links statically, so the configuration needs `libc.a` and
`libstdc++.a` (Fedora: `glibc-static` and `libstdc++-static`). Without them, the configuration stops with a message
that names the packages. Do not install them: tell the user, or configure with `-DAFTERGLOW_BUILD_AGLOW=OFF`. Then
the `aglow` tests are left out, and the `aglow dnd` tests of `afterglow_dnd_tests` and the native suite skip.
`aglow` builds next to `afterglow` (`build/afterglow/aglow`), and building `afterglow` builds it too.
`tools/aglow/stb/stb_image.h` is pinned by its SHA-256 in `cmake/AfterglowDependencies.cmake`: after a change to the
file, set the new hash there and list the patch at the top of `tools/aglow/StbImage.cpp`.

`AFTERGLOW_BUILD_TOOLS=ON` builds `afterglow_renderpng`, `afterglow_headless`, `afterglow_vtbench` and
`afterglow_native_bench`. The render tests and shader previews need it.

## The Phosphor libraries

`cmake/AfterglowPhosphor.cmake` compiles the Phosphor libraries from `../PlasmaZones/phosphor-libs`
(`AFTERGLOW_PHOSPHOR_LIBS_DIR`). Thus the state of that checkout is part of the Afterglow build:

- If the PlasmaZones checkout is on another branch, or has uncommitted work, an Afterglow build can fail or change
  for reasons that are not in this repository. Check `git -C ../PlasmaZones status -sb` before you blame Afterglow.
- Never edit PlasmaZones from an Afterglow session. A Phosphor change goes through a PlasmaZones branch and pull
  request. Tell the user what Phosphor needs.
- The renderer uses Qt private APIs. After a Qt update, rebuild everything (`cmake --build build --clean-first`).
- The pinned archives (Luau and valijson from `PhosphorDependencies.cmake` in PlasmaZones, zlib-ng and xxHash from
  `cmake/AfterglowDependencies.cmake`) download into the CPM cache, by default `../PlasmaZones/.cache/cpm`, and
  Afterglow's own `.cache/cpm` when only `PHOSPHOR_DEPS_ARCHIVES` is set (`docs/BUILD.md` has the order). A
  configure without network works once that cache holds them.

## Build

```sh
cmake --build build -j6 2>&1 | tee <scratchpad>/build.log | tail -5
grep -c "warning:" <scratchpad>/build.log
```

`<scratchpad>` is the session scratchpad directory. Never write logs into the repository.

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
ctest --test-dir build --output-on-failure -j6 2>&1 | tail -15
```

The suite has about 1,350 tests. All of them must pass. Use `-R PATTERN` while you work, and run the whole suite
before a commit.

- `aglow_only_build` (label `slow`) configures, builds and installs `aglow` alone (`AFTERGLOW_AGLOW_ONLY`) in
  `build/tools/aglow/aglow-only`, with Qt hidden and no Phosphor directory. It compiles zlib-ng again and takes some
  minutes. `-LE slow` leaves it out while you work. The whole suite before a commit includes it.

- The renderer tests (`libs/afterglow-render/tests`, the `Shaders/` tests) need a working Vulkan driver.
- The native window tests (`afterglow_native_tests`) are not in CTest by default (`AFTERGLOW_NATIVE_TESTS=OFF`).
  They need a nested KWin. Use the `ag-verify-live` skill to run them.
- The Python suites in `afterglow/tests/*.py` are separate. Run the ones that cover the area that you changed
  (`docs/TESTING.md` lists them). CTest runs `tools/conformance/test_headless.py` as `afterglow_headless_runner`.

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
