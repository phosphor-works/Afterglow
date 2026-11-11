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
- libpng, libjpeg-turbo 3.0 or later and libwebp (with libwebpdemux), for `aglow` and `afterglow-mux`. Qt reads images with the same libraries.
- xkbcommon, wayland-client, `wayland-scanner` and the 32-bit PCRE2 library.
- KF6 WindowSystem.
- GTest, unless you configure with `-DBUILD_TESTING=OFF`.
- The Phosphor libraries from a PlasmaZones checkout.

To build without `aglow`, configure with `-DAFTERGLOW_BUILD_AGLOW=OFF`.
Then `afterglow run`, `afterglow transfer`, `afterglow dnd`, `afterglow icat` and `afterglow clipboard` show an error and exit with status 127.
A shell cannot run script commands with their arguments, standard input and exit status.
`afterglow ctl action script:ID.COMMAND` still starts a command, with no arguments.
The shell completions offer no script commands. For these five commands, they complete file names.
A build with `-DAFTERGLOW_BUILD_FUZZERS=ON` does not build `aglow`.

The build compiles zlib-ng and xxHash from their [source archives](#source-archives).
These are C sources, so the build needs the C compiler.
It does not use the copies that the system has.

| Optional software | Function | Result when it is missing |
| --- | --- | --- |
| `rsvg-convert` | Creates the PNG icon sizes | Only the SVG icon is installed. |
| `tic` | Compiles the `afterglow` terminfo entry | The entry is not installed. Do not set `Terminal.term` to `afterglow`. |
| `cava` | Supplies system audio data for shaders | Audio-reactive shaders get no audio. |
| KF6 GlobalAccel | Registers the quick terminal shortcut with KDE global shortcuts | The desktop portal registers the shortcut. |
| `qqc2-desktop-style` | Draws tabs, menus and prompts in the Plasma style | The controls use the Fusion style. |
| `bash-completion` | Loads the Bash completions of `afterglow` and `aglow` | Bash does not complete `afterglow` and `aglow` arguments. |

## Source archives

The first configuration downloads source archives. It needs network access one time.

| Archive | Pin file | Function |
| --- | --- | --- |
| CPM.cmake | Both pin files | Downloads the other archives |
| Luau | `../PlasmaZones/cmake/PhosphorDependencies.cmake` | Runs the Luau scripts |
| valijson | `../PlasmaZones/cmake/PhosphorDependencies.cmake` | Checks Phosphor JSON files against their schemas |
| zlib-ng | `cmake/AfterglowDependencies.cmake` | Compresses and decompresses images and file transfers |
| xxHash | `cmake/AfterglowDependencies.cmake` | Calculates the block hashes of delta file transfers |

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
This build needs CMake 3.21 or later, pkg-config, a C compiler, a C++20 compiler, libpng, libjpeg-turbo 3.0 or later and libwebp.
It does not need Qt, the other libraries of Afterglow or a PlasmaZones checkout.
It compiles zlib-ng and xxHash from their [source archives](#source-archives), as the full build does.
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
It installs the [mux daemon](MUX.md) `afterglow-mux` in the same directory as `afterglow`, because `afterglow mux` runs it from there.
The Fish completions need Fish 4 or later.
The license notices of the code in `aglow` go in `share/licenses/afterglow`:

| File | Contents | Installed |
| --- | --- | --- |
| `LICENSE.zlib-ng` | The notice of zlib-ng, which `afterglow` also contains | Always |
| `LICENSE.xxHash` | The notice of xxHash, which `afterglow` also contains | Always |

`aglow` and `afterglow-mux` link libpng, libjpeg-turbo and libwebp of the system, which come with their own notices.
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
