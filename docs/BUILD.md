<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Build and install

Afterglow requires Linux, Wayland and a Vulkan driver.
These instructions use the current source layout.
To build `aglow` alone, without Qt and the Phosphor libraries, see [build aglow alone](#build-aglow-alone).

## Required software

- CMake 3.21 or later, pkg-config, a C compiler and a C++20 compiler.
- Qt 6.10 or later. The Phosphor libraries need this version.
- Qt Core, Gui, Widgets, Qml, Quick, QuickControls2, Concurrent, DBus, Network, Svg, WaylandClient and ShaderTools.
- Qt CorePrivate, GuiPrivate, ShaderToolsPrivate, WaylandClientPrivate and QuickPrivate headers.
- FreeType, HarfBuzz, FriBidi and fontconfig.
- xkbcommon, wayland-client, `wayland-scanner` and the 32-bit PCRE2 library.
- KF6 WindowSystem.
- GTest, unless you configure with `-DBUILD_TESTING=OFF`.
- The Phosphor libraries from a PlasmaZones checkout.
- The static C and C++ runtime libraries (`libc.a` and `libstdc++.a`), for `aglow`.
  Fedora puts them in the packages `glibc-static` and `libstdc++-static`.
  Without them, the configuration stops with a message.
  To build without `aglow`, configure with `-DAFTERGLOW_BUILD_AGLOW=OFF`.
  Then `afterglow run`, `afterglow transfer`, `afterglow dnd`, `afterglow icat` and `afterglow clipboard` show an error and exit with status 127.
  A shell cannot run script commands with their arguments, standard input and exit status.
  `afterglow ctl action script:ID.COMMAND` still starts a command, with no arguments.
  The shell completions offer no script commands. For these five commands, they complete file names.
  A build with `-DAFTERGLOW_BUILD_FUZZERS=ON` does not build `aglow`.

The build compiles zlib-ng, xxHash, libpng, libjpeg-turbo and libwebp from their [source archives](#source-archives).
These are C sources, so the build needs the C compiler.
It does not use the copies that the system has.
The libjpeg-turbo archive also holds libspng and zlib, for image files of its own API.
`aglow` and `aglow-mux` do not use them, but they are in the library that the build links.
The notice of libjpeg-turbo covers them, and a fix of either comes with a release of libjpeg-turbo.

| Optional software | Function | Result when it is missing |
| --- | --- | --- |
| `rsvg-convert` | Creates the PNG icon sizes | Only the SVG icon is installed. |
| `tic` | Compiles the `afterglow` terminfo entry | The entry is not installed. Do not set `Terminal.term` to `afterglow`. |
| `cava` | Supplies system audio data for shaders | Audio-reactive shaders get no audio. |
| KF6 GlobalAccel | Registers the quick terminal shortcut with KDE global shortcuts | The desktop portal registers the shortcut. |
| `qqc2-desktop-style` | Draws tabs, menus and prompts in the Plasma style | The controls use the Fusion style. |
| `bash-completion` | Loads the Bash completions of `afterglow` and `aglow` | Bash does not complete `afterglow` and `aglow` arguments. |
| NASM | Builds the SIMD code of libjpeg-turbo on x86-64 computers | `aglow` and `aglow-mux` decode JPEG images more slowly. The configuration says so. |
| Qt Image Formats | Decodes WebP images that a program shows in a pane outside a session | Such a pane does not show WebP images. A session pane shows them. |
| Python 3 (3.9 or later for the tests of `aglow`) | Runs the Python tests in CTest | These tests are not added to CTest. The configuration names the tests of `aglow` that it leaves out. With `AFTERGLOW_NATIVE_TESTS=ON`, the configuration stops with an error. |

## Source archives

The first configuration downloads source archives. It needs network access one time.

| Archive | Pin file | Function |
| --- | --- | --- |
| CPM.cmake | Both pin files | Downloads the other archives |
| Luau | `../PlasmaZones/cmake/PhosphorDependencies.cmake` | Runs the Luau scripts |
| valijson | `../PlasmaZones/cmake/PhosphorDependencies.cmake` | Checks Phosphor JSON files against their schemas |
| zlib-ng | `cmake/AfterglowDependencies.cmake` | Compresses and decompresses images and file transfers |
| xxHash | `cmake/AfterglowDependencies.cmake` | Calculates the block hashes of delta file transfers |
| libpng | `cmake/AfterglowDependencies.cmake` | Decodes PNG images in `aglow` and `aglow-mux` |
| libjpeg-turbo | `cmake/AfterglowDependencies.cmake` | Decodes JPEG images in `aglow` and `aglow-mux` |
| libwebp | `cmake/AfterglowDependencies.cmake` | Decodes WebP images and animations in `aglow` and `aglow-mux` |

CMake checks each archive against the SHA-256 hash in its pin file.
The archives go into the CPM cache.
By default, every build directory uses the same cache.
The first row of this table that applies selects the cache:

| Condition | CPM cache |
| --- | --- |
| The configuration command has `-DCPM_SOURCE_CACHE=/path/to/cache` | `/path/to/cache` |
| The `CPM_SOURCE_CACHE` environment variable is set | The directory in the variable |
| The configuration command has `-DPHOSPHOR_DEPS_ARCHIVES` and no `-DAFTERGLOW_DEPS_ARCHIVES` | `.cache/cpm` in the Afterglow directory |
| No other row applies | `.cache/cpm` in the PlasmaZones checkout |

A build directory keeps the CPM cache of its first configuration.
To change the cache of a build directory, give `-DCPM_SOURCE_CACHE`.
When the cache has all archives, a configuration needs no network access.

To build on a computer without network access, prepare the source on a computer with network access:

1. Do steps 1 to 3 of [Build](#build).

2. In the Afterglow directory, download the Phosphor archives into a directory:

```sh
cmake -DPHOSPHOR_DEPS_FETCH=/path/to/archives -P ../PlasmaZones/cmake/PhosphorDependencies.cmake
```

3. Download the Afterglow archives into the same directory:

```sh
cmake -DAFTERGLOW_DEPS_FETCH=/path/to/archives -P cmake/AfterglowDependencies.cmake
```

This command also puts an empty `.lock` file next to each Afterglow archive that it downloads. You do not need to copy these files.
To download the archives in another way, give each one the `FILE` name of its pin in `cmake/AfterglowDependencies.cmake`.
The pin file also has the URL and the SHA-256 hash of each archive.

4. Copy the Afterglow and PlasmaZones directories into one parent directory on the build computer.

5. Copy the archive directory to the build computer.

6. On the build computer, configure the build as in step 4 of [Build](#build), with these options:

```sh
-DPHOSPHOR_DEPS_ARCHIVES=/path/to/archives -DAFTERGLOW_DEPS_ARCHIVES=/path/to/archives
```

7. Do steps 5 and 6 of [Build](#build).

With the two options, the configuration downloads nothing.
If you set only one option, the configuration downloads CPM.cmake and the archives of the other pin file.

## Qt private APIs

Afterglow uses Qt private APIs. CMake reports this dependency during configuration.
These APIs are tied to the installed Qt build.
Rebuild Afterglow after a Qt update.
Do not run an existing binary against a different Qt private-module build.

## Build

1. Get the Afterglow source:

```sh
git clone https://github.com/phosphor-works/Afterglow
cd Afterglow
```

2. Get the Phosphor source in the adjacent directory:

```sh
git clone https://github.com/phosphor-works/PlasmaZones ../PlasmaZones
```

3. Select the PlasmaZones commit that this Afterglow source was tested with:

```sh
git -C ../PlasmaZones checkout f8a408aae
```

A later PlasmaZones commit can change the Phosphor API. The build then fails.
An older PlasmaZones checkout lacks `cmake/PhosphorDependencies.cmake`, or a library such as `phosphor-channel`. The configuration then stops with an error.

4. Configure the build with the installation prefix:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
```

The executable stores the shell integration, terminfo, shader, theme and plugin paths of this prefix.
Without GTest, add `-DBUILD_TESTING=OFF` to the command, and skip step 6.

5. Build the application:

```sh
cmake --build build
```

6. Run the tests:

```sh
ctest --test-dir build --output-on-failure
```

To use another Phosphor checkout, add this option to the configuration command:

```sh
-DAFTERGLOW_PHOSPHOR_LIBS_DIR=/path/to/PlasmaZones/phosphor-libs
```

The build compiles the Phosphor libraries with Afterglow.

## Build aglow alone

Build `aglow` alone on a computer that does not run Afterglow, for example a remote host.
A distribution can also package `aglow` on its own from this build.
This build needs CMake 3.21 or later, a C compiler, a C++20 compiler and the static C and C++ runtime libraries.
NASM is optional, as in the full build.
It does not need Qt, the other libraries of Afterglow or a PlasmaZones checkout.
It compiles zlib-ng, xxHash, libpng, libjpeg-turbo and libwebp from their [source archives](#source-archives), as the full build does.
It downloads only CPM.cmake and the archives of `cmake/AfterglowDependencies.cmake`.
By default, the CPM cache is `.cache/cpm` in the Afterglow directory.
`-DCPM_SOURCE_CACHE` and the `CPM_SOURCE_CACHE` environment variable change it.
To build on a computer without network access, download the archives as in step 3 of the procedure in [source archives](#source-archives).
Copy the archive directory to the build computer.
Then add `-DAFTERGLOW_DEPS_ARCHIVES=/path/to/archives` to the configuration command.

1. Get the Afterglow source:

```sh
git clone https://github.com/phosphor-works/Afterglow
cd Afterglow
```

2. Configure the build with `AFTERGLOW_AGLOW_ONLY`:

```sh
cmake -B build-aglow -DAFTERGLOW_AGLOW_ONLY=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
```

Without GTest, add `-DBUILD_TESTING=OFF` to the command, and skip step 4.

3. Build `aglow`:

```sh
cmake --build build-aglow
```

4. Run the tests:

```sh
ctest --test-dir build-aglow --output-on-failure
```

5. Install `aglow`:

```sh
sudo cmake --install build-aglow
```

The installation contains `bin/aglow`, its license notices in `share/licenses/aglow`, and its Bash, Zsh and Fish completions.
The license notices are those of zlib-ng, xxHash, libpng, libjpeg-turbo (with `README.ijg` and `NOTICE.IJG`) and libwebp in the table in [install](#install).
A static `aglow` also has the notices of glibc and libstdc++.
The texts of the licenses that they name are also installed. These are `LGPL-2.1-or-later.txt`, and `GPL-3.0-or-later.txt` for a static `aglow`.
The completions go in the directories of the full build (see [install](#install)), and `AFTERGLOW_ZSH_COMPLETION_DIR` also applies.
It contains no other part of Afterglow.

## Install

Run this command to install the build:

```sh
sudo cmake --install build
```

Use the prefix of the configuration step. Do not give a different `--prefix` here.
If you do, shell integration is off, and the executable can miss its plugin, shader packs and themes.

The installation includes the executable, desktop entry, AppStream metadata, icons, shader packs, themes and terminfo entry.
It also includes the shell integration scripts, the API declarations, the examples and the license texts.
It also includes the shell completions for Bash, Zsh and Fish.
It installs `aglow` in the same directory as `afterglow`, with its Bash, Zsh and Fish completions.
It installs the [mux daemon](MUX.md) `aglow-mux` in the same directory as `afterglow`, because `afterglow mux` runs it from there.
The Fish completions need Fish 4 or later.
The license notices of the third-party code in the binaries go in `share/licenses/afterglow`:

| File | Contents | Installed |
| --- | --- | --- |
| `LICENSE.zlib-ng` | The notice of zlib-ng, which `afterglow` also contains | Always |
| `LICENSE.xxHash` | The notice of xxHash, which `afterglow` also contains | Always |
| `LICENSE.libpng` | The notice of libpng, which `aglow-mux` also contains | Always |
| `LICENSE.libjpeg-turbo` | The notice of libjpeg-turbo, which `aglow-mux` also contains | Always |
| `README.ijg` | The license of the Independent JPEG Group, which the notice of libjpeg-turbo names | Always |
| `LICENSE.libwebp` | The notice of libwebp, which `aglow-mux` also contains | Always |
| `PATENTS.libwebp` | The patent grant of libwebp | Always |
| `NOTICE.IJG` | The statement that `aglow` and `aglow-mux` are based in part on the work of the Independent JPEG Group | Always |
| `LICENSE.Luau` and `LICENSE.Lua` | The notices of Luau and of Lua, which the scripts of `afterglow` run with | Unless the build uses the system's Luau (`PLASMAZONES_SYSTEM_LUAU`) |
| `LICENSE.valijson` | The notice of valijson, which `afterglow` checks JSON files with | Unless the build uses the system's valijson (`PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON`) |
| `LICENSE.glibc` | The notice of the GNU C Library, with the steps to link `aglow` again with another C library | When `aglow` is static |
| `LICENSE.libstdc++` | The notice of libstdc++ and libgcc, with the GCC Runtime Library Exception | When `aglow` is static |

`aglow` and `aglow-mux` are based in part on the work of the Independent JPEG Group.
A static `aglow` contains parts of glibc, which is LGPL-2.1-or-later.
The build installs `LICENSE.glibc` and `LICENSE.libstdc++` only when the C library is glibc and the C++ library is libstdc++. With another library, the configuration writes a warning, and its notice is the packager's to add.
A static `aglow` loads at a random address when the C library supports it (`-static-pie`).
To link `aglow` again with a changed C library, build the `aglow` target from the Afterglow source of the same version (`cmake --build build --target aglow`).
A [build of aglow alone](#build-aglow-alone) also does this, without Qt and the Phosphor libraries.
When `readelf` is installed, the build of a static `aglow` prints the oldest Linux kernel that it needs, and on x86-64 the processor level.
When the compiler or linker flags contain `-fsanitize`, such as in the `debug` preset, `aglow` links dynamically.
A static program cannot use the sanitizer runtimes.
Private libraries go in an `afterglow` directory in the library directory, for example `lib/afterglow/` or `lib64/afterglow/`.
This location does not replace libraries from a system Phosphor installation.

| Shell | Completion file of `afterglow` | Completion file of `aglow` |
| --- | --- | --- |
| Bash | `share/bash-completion/completions/afterglow` | `share/bash-completion/completions/aglow` |
| Zsh | `share/zsh/site-functions/_afterglow` | `share/zsh/site-functions/_aglow` |
| Fish | `share/fish/vendor_completions.d/afterglow.fish` | `share/fish/vendor_completions.d/aglow.fish` |

Zsh reads only the directories in its `fpath`.
When the prefix is `/usr` on Debian or Ubuntu, `share/zsh/site-functions` is not in `fpath`.
For this prefix on these systems, add this option to the configuration command. It applies to `_afterglow` and `_aglow`:

```sh
-DAFTERGLOW_ZSH_COMPLETION_DIR=share/zsh/vendor-completions
```

See [testing](TESTING.md) for native desktop test commands.
