<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Build and install

Afterglow requires Linux, Wayland and a Vulkan driver.
These instructions use the current source layout.
To build `aglow` and `aglow-mux` alone, without Qt, see [build aglow and aglow-mux alone](#build-aglow-and-aglow-mux-alone).

## Required software

- CMake 3.21 or later, pkg-config, a C compiler and a C++20 compiler.
- Qt 6.10 or later. The Phosphor libraries need this version.
- Qt Core, Gui, Widgets, Qml, Quick, QuickControls2, Concurrent, DBus, Network, Svg, WaylandClient and ShaderTools.
- Qt CorePrivate, GuiPrivate, ShaderToolsPrivate, WaylandClientPrivate and QuickPrivate headers.
- FreeType, HarfBuzz, FriBidi and fontconfig.
- xkbcommon, wayland-client, `wayland-scanner` and the 32-bit PCRE2 library.
- KF6 WindowSystem.
- GTest, unless you configure with `-DBUILD_TESTING=OFF`.
- The static C and C++ runtime libraries (`libc.a` and `libstdc++.a`), for `aglow`.
  Fedora puts them in the packages `glibc-static` and `libstdc++-static`.
  When the compiler does not search their directories, give them in `-DAFTERGLOW_STATIC_RUNTIME_DIRS`, separated by `;`.
  Only the link of `aglow` searches these directories.
  Without them, the configuration stops with a message.
  To build without `aglow`, configure with `-DAFTERGLOW_BUILD_AGLOW=OFF`.
  Then `afterglow run`, `afterglow transfer`, `afterglow dnd`, `afterglow icat` and `afterglow clipboard` show an error and exit with status 127.
  A shell cannot run script commands with their arguments, standard input and exit status.
  `afterglow ctl action script:ID.COMMAND` still starts a command, with no arguments.
  The shell completions offer no script commands. For these five commands, they complete file names.
  A build with `-DAFTERGLOW_BUILD_FUZZERS=ON` does not build `aglow`.

The build compiles the Phosphor libraries, zlib-ng, xxHash, libpng, libjpeg-turbo, libwebp and Little CMS from their [source archives](#source-archives).
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
| ImageMagick 7 (`magick`), at run time | Lets `aglow icat` show AVIF, HEIC, JPEG XL, QOI, SVG, TGA, ICO and CUR images. With it, `aglow icat` also shows a BMP with JPEG or PNG data, and a BMP with run-length encoding that does not fit its depth. See [other formats and URLs](DESKTOP.md#other-formats-and-urls). | `aglow icat` refuses these images with a message. |
| `curl`, at run time | Lets `aglow icat` show images from `http`, `https` and `ftp` URLs | `aglow icat` refuses these URLs with a message. |
| NASM | Builds the SIMD code of libjpeg-turbo on x86-64 computers | `aglow` and `aglow-mux` decode JPEG images more slowly. The configuration says so. |
| Python 3 (3.9 or later for the tests of `aglow`) | Runs the Python tests in CTest | These tests are not added to CTest. The configuration names the tests of `aglow` that it leaves out. With `AFTERGLOW_NATIVE_TESTS=ON`, the configuration stops with an error. |

## Source archives

The first configuration downloads source archives. It needs network access one time.

| Archive | Pin file | Function |
| --- | --- | --- |
| CPM.cmake | `cmake/AfterglowDependencies.cmake` | Downloads the other archives |
| phosphor-libs | `cmake/AfterglowDependencies.cmake` | Gives the Phosphor libraries. The pin is a commit of phosphor-libs |
| Luau | `cmake/PhosphorDependencies.cmake` in phosphor-libs | Runs the Luau scripts |
| jsoncons | `cmake/PhosphorDependencies.cmake` in phosphor-libs | Checks Phosphor JSON files against their schemas |
| zlib-ng | `cmake/AfterglowDependencies.cmake` | Compresses and decompresses images and file transfers |
| xxHash | `cmake/AfterglowDependencies.cmake` | Calculates the block hashes of delta file transfers |
| libpng | `cmake/AfterglowDependencies.cmake` | Decodes PNG images in `aglow` and `aglow-mux` |
| libjpeg-turbo | `cmake/AfterglowDependencies.cmake` | Decodes JPEG images in `aglow` and `aglow-mux` |
| libwebp | `cmake/AfterglowDependencies.cmake` | Decodes WebP images and animations in `aglow` and `aglow-mux` |
| Little CMS (lcms2) | `cmake/AfterglowDependencies.cmake` | Converts images with an ICC profile or a cICP chunk to sRGB in `aglow` and `aglow-mux` |

CMake checks each archive against the SHA-256 hash in its pin file.
The archives go into the CPM cache.
By default, every build directory uses the same cache.
The first row of this table that applies selects the cache:

| Condition | CPM cache |
| --- | --- |
| The configuration command has `-DCPM_SOURCE_CACHE=/path/to/cache` | `/path/to/cache` |
| The `CPM_SOURCE_CACHE` environment variable is set | The directory in the variable |
| No other row applies | `.cache/cpm` in the Afterglow directory |

A build directory keeps the CPM cache of its first configuration.
To change the cache of a build directory, give `-DCPM_SOURCE_CACHE`.
When the cache has all archives, a configuration needs no network access.

To build on a computer without network access, prepare the source on a computer with network access:

1. Do step 1 of [Build](#build).

2. In the Afterglow directory, download the archives into a directory:

```sh
cmake -DAFTERGLOW_DEPS_FETCH=/path/to/archives -P cmake/AfterglowDependencies.cmake
```

This command downloads the archives of both pin files.
It also puts an empty `.lock` file next to each Afterglow archive that it downloads. You do not need to copy these files.
To download the archives in another way, give each one the `FILE` name of its pin.
The pin files also have the URL and the SHA-256 hash of each archive.

3. Copy the Afterglow directory and the archive directory to the build computer.

4. On the build computer, configure the build as in step 2 of [Build](#build), with this option:

```sh
-DAFTERGLOW_DEPS_ARCHIVES=/path/to/archives
```

5. Do steps 3 and 4 of [Build](#build).

With this option, the configuration downloads nothing.

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

2. Configure the build with the installation prefix:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
```

The executable stores the shell integration, terminfo, shader, theme and plugin paths of this prefix.
Without GTest, add `-DBUILD_TESTING=OFF` to the command, and skip step 4.

3. Build the application:

```sh
cmake --build build
```

4. Run the tests:

```sh
ctest --test-dir build --output-on-failure
```

The build compiles the Phosphor libraries with Afterglow, from the phosphor-libs commit that `cmake/AfterglowDependencies.cmake` pins.
To use a phosphor-libs checkout instead, add this option to the configuration command:

```sh
-DAFTERGLOW_PHOSPHOR_LIBS_DIR=/path/to/phosphor-libs
```

Use this option to build against a change to phosphor-libs before the pin moves to it.

## Build with Nix

The flake builds Afterglow on `x86_64-linux` and `aarch64-linux`.
It fetches the archives that `cmake/AfterglowDependencies.cmake` pins, and the archives of the pinned phosphor-libs commit.

| Command | Result |
| --- | --- |
| `nix build .#default` | Builds the package into `result` |
| `nix flake check` | Builds the package and runs the tests in the sandbox. The sandbox has no Wayland display, so the tests that draw with Vulkan report Skipped. |
| `nix develop` | Opens a shell with the build dependencies, `ccache` and `clang-tools` |

## Build aglow and aglow-mux alone

Build `aglow` and the [mux daemon](MUX.md) `aglow-mux` alone on a computer that does not run Afterglow, for example a remote host.
The `--ssh DESTINATION` option of `afterglow mux` runs the `aglow-mux` of the remote host.
A distribution can also package `aglow` and `aglow-mux` on their own from this build.
This build needs CMake 3.21 or later, a C compiler, a C++20 compiler and the static C and C++ runtime libraries.
NASM is optional, as in the full build.
It does not need Qt.
It compiles zlib-ng, xxHash, libpng, libjpeg-turbo, libwebp and Little CMS from their [source archives](#source-archives), as the full build does.

`aglow-mux` also needs:

| Dependency | Use |
| --- | --- |
| PCRE2 with 32-bit code units (`libpcre2-32`), and `pkg-config` | The terminal core, which `aglow-mux` uses, links it for regular expression search |
| The pinned phosphor-libs archive, which the configuration downloads | Gives phosphor-channel, which carries the frames of a session to its window. The build uses no other library of phosphor-libs. |

This build also works on FreeBSD 15 with the clang of the base system.
The packages are `cmake-core`, `ninja`, `pkgconf`, `pcre2` and `nasm`, and `googletest` and `qt6-base` for the tests.
The base system has the static C and C++ libraries.
On FreeBSD, `aglow` is static but does not load at a random address, because the C library has no start file for `-static-pie`.
The image decoders of `aglow` and `aglow-mux` run in Capsicum capability mode, where Linux uses a seccomp filter.

To build `aglow` without `aglow-mux`, add `-DAFTERGLOW_BUILD_MUX=OFF` to the configuration command.
That build does not need PCRE2 or phosphor-libs.

This build, with or without `aglow-mux`, downloads only CPM.cmake and the archives of `cmake/AfterglowDependencies.cmake`.
phosphor-libs is one of them. phosphor-channel downloads nothing more.
By default, the CPM cache is `.cache/cpm` in the Afterglow directory.
`-DCPM_SOURCE_CACHE` and the `CPM_SOURCE_CACHE` environment variable change it.
To build on a computer without network access, download the archives as in step 2 of the procedure in [source archives](#source-archives).
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

To use a phosphor-libs checkout for `aglow-mux`, add `-DAFTERGLOW_PHOSPHOR_LIBS_DIR=/path/to/phosphor-libs` to the command.
Without GTest, add `-DBUILD_TESTING=OFF` to the command, and skip step 4.

3. Build `aglow` and `aglow-mux`:

```sh
cmake --build build-aglow
```

4. Run the tests:

```sh
ctest --test-dir build-aglow --output-on-failure
```

5. Install `aglow` and `aglow-mux`:

```sh
sudo cmake --install build-aglow
```

The installation contains `bin/aglow`, its license notices in `share/licenses/aglow`, and its Bash, Zsh and Fish completions.
The license notices are those of zlib-ng, xxHash, libpng, libjpeg-turbo (with `README.ijg` and `NOTICE.IJG`), libwebp, Little CMS and the Unicode Character Database (`LICENSE.Unicode`).
The table in [install](#install) lists them.
A static `aglow` also has the notices of glibc and libstdc++.
The texts of the licenses that they name are also installed. These are `LGPL-2.1-or-later.txt`, and `GPL-3.0-or-later.txt` for a static `aglow`.
The completions go in the directories of the full build (see [install](#install)), and `AFTERGLOW_ZSH_COMPLETION_DIR` also applies.

The installation also contains `bin/aglow-mux` and its license notices in `share/licenses/aglow-mux`.
These are the notices of `aglow`, without those of glibc and libstdc++, and the texts of `GPL-3.0-or-later.txt` and `LGPL-2.1-or-later.txt`.
`aglow-mux` is not static. It uses the C and C++ runtime libraries and PCRE2 of the system.
Its private libraries `libAfterglowVt`, `libAfterglowPty` and `libPhosphorChannel` go in the `afterglow` directory of the library directory, as in the full build.
The installation contains no other part of Afterglow.

## Install

Run this command to install the build:

```sh
sudo cmake --install build
```

Use the prefix of the configuration step. Do not give a different `--prefix` here.
If you do, shell integration is off, and the executable can miss its plugin, shader packs and themes.

The installation includes the executable, desktop entry, AppStream metadata, icons, shader packs, themes and terminfo entry.
It also includes the shell integration scripts, the API declarations and the license texts.
The [examples](../examples/README.md) stay in the source repository.
It also includes the shell completions for Bash, Zsh and Fish.
It installs `aglow` in the same directory as `afterglow`, with its Bash, Zsh and Fish completions.
It installs the [mux daemon](MUX.md) `aglow-mux` in the same directory as `afterglow`, because `afterglow mux` runs it from there.
A pane also decodes the images that programs send with the `aglow-mux` next to `afterglow`. Without it, these images do not show.
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
| `LICENSE.lcms2` | The notice of Little CMS, which `aglow-mux` also contains | Always |
| `LICENSE.Unicode` | The Unicode License v3 of the Unicode Character Database, from which the character width and grapheme tables of `afterglow` and `aglow-mux` come | Always |
| `NOTICE.IJG` | The statement that `aglow` and `aglow-mux` are based in part on the work of the Independent JPEG Group | Always |
| `LICENSE.Luau` and `LICENSE.Lua` | The notices of Luau and of Lua, which the scripts of `afterglow` run with | Unless the build uses the system's Luau (`PHOSPHOR_SYSTEM_LUAU`) |
| `LICENSE.jsoncons` | The notice of jsoncons, which `afterglow` checks JSON files with. The MIT notices of the grisu3 and a5hash code in jsoncons follow it | Unless the build uses the system's jsoncons (`PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS`) |
| `LICENSE.glibc` | The notice of the GNU C Library, with the steps to link `aglow` again with another C library | When `aglow` is static |
| `LICENSE.libstdc++` | The notice of libstdc++ and libgcc, with the GCC Runtime Library Exception | When `aglow` is static |

`aglow` and `aglow-mux` are based in part on the work of the Independent JPEG Group.
A static `aglow` contains parts of glibc, which is LGPL-2.1-or-later.
The build installs `LICENSE.glibc` and `LICENSE.libstdc++` only when the C library is glibc and the C++ library is libstdc++. With another library, the configuration writes a warning, and its notice is the packager's to add.
A static `aglow` loads at a random address when the C library supports it (`-static-pie`).
To link `aglow` again with a changed C library, build the `aglow` target from the Afterglow source of the same version (`cmake --build build --target aglow`).
A [build of aglow and aglow-mux alone](#build-aglow-and-aglow-mux-alone) also does this, without Qt.
When `readelf` is installed, the build of a static `aglow` prints the oldest Linux kernel that it needs, and on x86-64 the processor level.
When the compiler or linker flags contain `-fsanitize`, such as in the `debug` preset, `aglow` links dynamically.
A static program cannot use the sanitizer runtimes.
Private libraries go in a `phosphor-works/afterglow` directory in the library directory, for example `lib/phosphor-works/afterglow/` or `lib64/phosphor-works/afterglow/`.
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
